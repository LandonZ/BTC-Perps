#include "maker-backtest.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace btc::maker_backtest {
namespace {
constexpr double bps = 10000;
double number(const std::string& s) {
    std::size_t used = 0;
    const double value = std::stod(s,&used);
    if (used != s.size() || !std::isfinite(value)) throw std::runtime_error("Invalid event number: "+s);
    return value;
}
std::vector<std::string> cells(const std::string& line) {
    std::vector<std::string> out(1);
    for (char ch : line) { if (ch == ',') out.emplace_back(); else if (ch != '\r') out.back() += ch; }
    return out;
}
}
Engine::Engine(Config config) : config_(config) {
    maker::validate(config.pricing);
    for (double v : {config.order_btc,config.max_inventory_btc,config.quantity_step,
                    config.min_notional,config.participation})
        if (!std::isfinite(v) || v <= 0) throw std::runtime_error("Replay sizes and participation must be finite and positive");
    if (config.participation > 1 || config.latency_ms < 0 || config.reprice_ms < 0 ||
        config.max_spot_age_ms < 0 || config.max_book_age_ms < 0 ||
        quantity(config.order_btc) < config.quantity_step)
        throw std::runtime_error("Invalid replay latency, participation, age or order quantity");
}
double Engine::quantity(double size) const {
    return std::floor(std::max(0.0,size)/config_.quantity_step+1e-9)*config_.quantity_step;
}
Equity Engine::totals() const {
    const double price = cash_+inventory_*(bid_+ask_)/2;
    return {now_,price,funding_,fees_,price+funding_-fees_,inventory_};
}
void Engine::activate(Millis time, bool inclusive) {
    for (int side = 0; side < 2; ++side) {
        auto& pending = pending_[side];
        if (!pending || pending->due > time || (!inclusive && pending->due == time)) continue;
        auto q = pending->quote;
        // Cancel-then-post, preserving the old quote until the replacement arrives.
        if (q.remaining > 0 && (bid_ <= 0 || time-book_time_ > config_.max_book_age_ms ||
            (side == 0 ? q.price >= ask_-config_.pricing.tick*1e-7 : q.price <= bid_+config_.pricing.tick*1e-7))) {
            q.remaining = 0; ++rejections_;
        }
        q.remaining = quantity(std::min(q.remaining,config_.max_inventory_btc+(side == 0 ? -inventory_ : inventory_)));
        q.since = pending->due;
        live_[side] = q;
        pending.reset();
    }
}
void Engine::trade(const Event& e) {
    if (config_.latency_ms == 0 && config_.pricing.rule == maker::QuoteRule::SpotFee && spot_ > 0 && bid_ > 0) {
        const double basis = ((bid_+ask_)/(2*spot_)-1)*bps;
        if (basis < config_.pricing.min_basis_bps-1e-9 || basis > config_.pricing.max_basis_bps+1e-9) {
            // References updated earlier in this timestamp are already known.
            // With zero latency, withdraw before the subsequent trade as well.
            for (int side = 0; side < 2; ++side) { live_[side] = {}; pending_[side].reset(); }
            return;
        }
    }
    const int side = e.buyer_maker ? 0 : 1;
    auto& q = live_[side];
    if (q.remaining <= 0 || e.time <= q.since) return;
    const double gap = side == 0 ? q.price-e.price : e.price-q.price;
    const double tolerance = config_.pricing.tick*1e-7;
    const bool through = gap > tolerance;
    if (!through && !(config_.fill_at_touch && std::abs(gap) <= tolerance)) return;
    const double capacity = config_.max_inventory_btc+(side == 0 ? -inventory_ : inventory_);
    const double size = quantity(std::min({q.remaining,e.quantity*config_.participation,capacity}));
    if (size <= 0) return;
    const int direction = side == 0 ? 1 : -1;
    const auto& pricing = config_.pricing;
    const double fee = size*(q.price*pricing.maker_fee_bps/bps+pricing.fee_usd_per_contract/pricing.contract_btc);
    inventory_ += direction*size;
    if (std::abs(inventory_) < 1e-10) inventory_ = 0;
    cash_ -= direction*size*q.price; fees_ += fee;
    q.remaining = quantity(q.remaining-size);
    fills_.push_back({now_,direction,q.price,size,fee,inventory_});
}
void Engine::decide() {
    maker::Quotes quotes;
    quotes.reason = "MISSING_MARKET";
    if (spot_ > 0 && bid_ > 0) {
        auto pricing = config_.pricing;
        pricing.funding_interval_minutes = interval_minutes_;
        maker::Inputs in;
        in.spot = spot_; in.perp_bid = bid_; in.perp_ask = ask_;
        in.funding_bps = rate_bps_; in.funding_ready = rate_time_ >= 0;
        in.inventory_contracts = inventory_/pricing.contract_btc;
        in.minutes_to_funding = next_funding_ > now_ ? (next_funding_-now_)/60000.0 : interval_minutes_;
        quotes = maker::price(in,pricing);
        if (now_-spot_time_ > config_.max_spot_age_ms || now_-book_time_ > config_.max_book_age_ms) {
            quotes.enabled = false; quotes.reason = "STALE_MARKET";
        } else if (pricing.rule == maker::QuoteRule::CostBudget && rate_time_ >= 0 && now_ >= next_funding_) {
            quotes.enabled = false; quotes.reason = "STALE_FUNDING";
        }
    }
    // Risk cancellations bypass the ordinary reprice throttle. In-flight
    // messages retain their arrival time instead of being postponed every tick.
    if (quotes.enabled && last_decision_ >= 0 && now_-last_decision_ < config_.reprice_ms) return;
    if (!quotes.enabled && !decisions_.empty() && decisions_.back().quotes.reason == quotes.reason &&
        !live_[0].remaining && !live_[1].remaining && !pending_[0] && !pending_[1]) return;
    last_decision_ = now_;
    const auto changed = [&] {
        if (decisions_.empty()) return true;
        const auto& old = decisions_.back(); const auto& q = old.quotes;
        return q.reason != quotes.reason || q.bid != quotes.bid || q.ask != quotes.ask ||
            q.bid_enabled != quotes.bid_enabled || q.ask_enabled != quotes.ask_enabled ||
            q.funding_reserve_bps != quotes.funding_reserve_bps ||
            old.inventory != inventory_ || old.funding_bps != rate_bps_;
    };
    if (changed()) decisions_.push_back({now_,quotes,rate_bps_,inventory_});
    for (int side = 0; side < 2; ++side) {
        Quote target;
        if (quotes.enabled && (side == 0 ? quotes.bid_enabled : quotes.ask_enabled)) {
            target.price = side == 0 ? quotes.bid : quotes.ask;
            target.remaining = quantity(std::min(config_.order_btc,
                config_.max_inventory_btc+(side == 0 ? -inventory_ : inventory_)));
            if (target.remaining*target.price < config_.min_notional) target.remaining = 0;
        }
        if (pending_[side]) continue;
        const auto& live = live_[side];
        if ((target.remaining > 0) == (live.remaining > 0) &&
            (target.remaining == 0 || target.price == live.price)) continue;
        pending_[side] = Pending{now_+config_.latency_ms,target};
    }
}
void Engine::on_event(const Event& e, bool decide_now) {
    if (e.time < 0 || e.time < now_) throw std::runtime_error("Replay events must be sorted by timestamp");
    for (double v : {e.price,e.quantity,e.bid,e.ask,e.funding_rate,e.interval_hours})
        if (!std::isfinite(v)) throw std::runtime_error("Non-finite replay event");
    activate(e.time);
    now_ = e.time;
    if (e.kind == 'S') {
        if (e.bid > 0 && e.ask >= e.bid) { spot_ = (e.bid+e.ask)/2; ++spot_mid_events_; }
        else {
            if (e.bid != 0 || e.ask != 0 || e.price <= 0) throw std::runtime_error("Invalid spot reference");
            spot_ = e.price; ++spot_proxy_events_;
        }
        spot_time_ = now_;
    } else if (e.kind == 'Q') {
        if (e.bid <= 0 || e.ask < e.bid) throw std::runtime_error("Invalid perp book");
        bid_ = e.bid; ask_ = e.ask; book_time_ = now_;
    } else if (e.kind == 'T') {
        if (e.price <= 0 || e.quantity <= 0) throw std::runtime_error("Invalid trade");
        trade(e);
    } else if (e.kind == 'F' || e.kind == 'R') {
        if (e.interval_hours <= 0 || (e.kind == 'F' && e.price <= 0)) throw std::runtime_error("Invalid funding event");
        if (e.kind == 'F') funding_ -= inventory_*e.price*e.funding_rate;
        rate_bps_ = e.funding_rate*bps; interval_minutes_ = e.interval_hours*60;
        rate_time_ = now_; next_funding_ = now_+static_cast<Millis>(interval_minutes_*60000);
    } else throw std::runtime_error("Unknown replay event");
    const bool instant_reference_update = config_.latency_ms == 0 &&
        config_.pricing.rule == maker::QuoteRule::SpotFee && (e.kind == 'S' || e.kind == 'Q');
    if (decide_now || instant_reference_update) {
        decide();
        if (config_.latency_ms == 0) activate(now_,true); // No artificial wait for a subsequent market event.
    }
    const auto point = totals();
    peak_ = std::max(peak_,point.net); drawdown_ = std::max(drawdown_,peak_-point.net);
    if (bid_ > 0 && (last_equity_ < 0 || now_/60000 > last_equity_/60000)) {
        equity_.push_back(point); last_equity_ = now_;
    }
}
void Engine::finish() {
    // Preserve and mark open inventory; do not invent a final maker fill.
    if (now_ >= 0) equity_.push_back(totals());
}
void read_events(const std::string& path, const std::function<void(const Event&,bool)>& feed) {
    std::ifstream file(path);
    if (!file) throw std::runtime_error("Cannot read events: "+path+"; run tools/download_binance_maker.py first");
    std::string line;
    std::getline(file,line);
    if (line.find("time_ms,kind,price,quantity,bid,ask,buyer_maker,funding_rate,interval_hours") != 0)
        throw std::runtime_error("Unexpected Binance replay header");
    std::optional<Event> previous;
    while (std::getline(file,line)) {
        if (line.empty()) continue;
        const auto c = cells(line);
        if (c.size() != 9 || c[1].size() != 1) throw std::runtime_error("Malformed replay row");
        Event e;
        std::size_t used = 0;
        e.time = std::stoll(c[0],&used);
        if (used != c[0].size()) throw std::runtime_error("Invalid replay timestamp");
        e.kind = c[1][0];
        if (!c[2].empty()) e.price = number(c[2]);
        if (!c[3].empty()) e.quantity = number(c[3]);
        if (!c[4].empty()) e.bid = number(c[4]);
        if (!c[5].empty()) e.ask = number(c[5]);
        if (e.kind == 'T' && c[6] != "true" && c[6] != "false") throw std::runtime_error("Trade must supply buyer_maker");
        e.buyer_maker = c[6] == "true";
        if (!c[7].empty()) e.funding_rate = number(c[7]);
        if (!c[8].empty()) e.interval_hours = number(c[8]);
        if ((e.kind == 'F' || e.kind == 'R') && (c[7].empty() || c[8].empty()))
            throw std::runtime_error("Funding event must supply a rate and interval");
        if (previous) feed(*previous,e.time != previous->time);
        previous = e;
    }
    if (!previous) throw std::runtime_error("Replay contains no events");
    feed(*previous,true);
}
void replay(Engine& engine, const std::string& path) {
    read_events(path,[&](const Event& e, bool decide) { engine.on_event(e,decide); });
    engine.finish();
}
void write_results(const Engine& engine, const std::string& directory) {
    std::filesystem::create_directories(directory);
    std::ofstream fills(directory+"/fills.csv"), quotes(directory+"/quotes.csv"), equity(directory+"/equity.csv");
    for (auto* stream : {&fills,&quotes,&equity}) *stream << std::fixed << std::setprecision(10);
    fills << "time_ms,side,price,btc,fee_usdt,inventory_btc\n";
    for (const auto& f : engine.fills())
        fills << f.time << ',' << (f.side > 0 ? "BUY" : "SELL") << ',' << f.price << ',' << f.btc << ',' << f.fee << ',' << f.inventory << '\n';
    quotes << "time_ms,status,spot_reservation,bid,ask,minimum_spread_bps,quoted_spread_bps,cycle_fees_bps,funding_reserve_bps,conditional_net_bps,funding_rate_bps,inventory_btc,observed_basis_bps,lower_price_bound,upper_price_bound,bid_enabled,ask_enabled\n";
    for (const auto& d : engine.decisions()) {
        const auto& q = d.quotes;
        quotes << d.time << ',' << q.reason << ',' << q.reference << ',' << q.bid << ',' << q.ask << ','
               << q.minimum_spread_bps << ',' << q.quoted_spread_bps << ',' << q.cycle_fees_bps << ','
               << q.funding_reserve_bps << ',' << q.expected_net_bps << ',' << d.funding_bps << ',' << d.inventory << ','
               << q.observed_basis_bps << ',' << q.lower_price_bound << ',' << q.upper_price_bound << ','
               << q.bid_enabled << ',' << q.ask_enabled << '\n';
    }
    equity << "time_ms,price_pnl,funding_pnl,fees,net_pnl,inventory_btc\n";
    for (const auto& p : engine.equity()) equity << p.time << ',' << p.price_pnl << ',' << p.funding << ',' << p.fees << ',' << p.net << ',' << p.inventory << '\n';
    const auto p = engine.totals(); const auto& c = engine.config();
    std::ofstream summary(directory+"/summary.json");
    summary << std::fixed << std::setprecision(10) << "{\n"
        << "  \"venue\": \"Binance USD-M BTCUSDT perpetual\",\n"
        << "  \"mode\": \"maker\",\n  \"maker_fee_bps\": " << c.pricing.maker_fee_bps << ",\n"
        << "  \"quote_rule\": \"" << (c.pricing.rule == maker::QuoteRule::SpotFee ? "spot-fee" : "cost-budget") << "\",\n"
        << "  \"spot_reference\": \"" << (engine.spot_proxy_events() ? "price proxy; not observed spot midpoint" : "observed spot bid/ask midpoint") << "\",\n"
        << "  \"spot_mid_events\": " << engine.spot_mid_events() << ",\n  \"spot_proxy_events\": " << engine.spot_proxy_events() << ",\n"
        << "  \"max_abs_funding_bps_per_settlement\": " << c.pricing.max_abs_funding_bps << ",\n"
        << "  \"min_basis_bps\": " << c.pricing.min_basis_bps << ",\n  \"max_basis_bps\": " << c.pricing.max_basis_bps << ",\n"
        << "  \"funding_signal\": \"" << (c.pricing.rule == maker::QuoteRule::SpotFee ? "basis gate only; recorded funding charged when due" : "last settled rate (not next realized rate)") << "\",\n"
        << "  \"funding_rate_filter_enabled\": " << (c.pricing.rule == maker::QuoteRule::CostBudget ? "true" : "false") << ",\n"
        << "  \"inventory_skew_bps_per_001_btc\": " << (c.pricing.rule == maker::QuoteRule::SpotFee ? 0 : c.pricing.skew_bps_per_contract) << ",\n"
        << "  \"horizon_minutes\": " << c.pricing.horizon_minutes << ",\n"
        << "  \"risk_buffer_bps\": " << c.pricing.risk_buffer_bps << ",\n"
        << "  \"profit_target_bps\": " << c.pricing.profit_bps << ",\n"
        << "  \"max_spread_bps\": " << c.pricing.max_spread_bps << ",\n"
        << "  \"tick\": " << c.pricing.tick << ",\n  \"quantity_step\": " << c.quantity_step << ",\n"
        << "  \"min_notional\": " << c.min_notional << ",\n"
        << "  \"order_btc\": " << c.order_btc << ",\n  \"max_inventory_btc\": " << c.max_inventory_btc << ",\n"
        << "  \"latency_ms\": " << c.latency_ms << ",\n  \"reprice_ms\": " << c.reprice_ms << ",\n"
        << "  \"participation\": " << c.participation << ",\n  \"fill_at_touch\": " << (c.fill_at_touch ? "true" : "false") << ",\n"
        << "  \"fills\": " << engine.fills().size() << ",\n  \"post_only_rejections\": " << engine.post_only_rejections() << ",\n"
        << "  \"price_pnl\": " << p.price_pnl << ",\n  \"funding_pnl\": " << p.funding << ",\n"
        << "  \"fees\": " << p.fees << ",\n  \"net_pnl\": " << p.net << ",\n  \"max_drawdown\": " << engine.drawdown() << ",\n"
        << "  \"final_inventory_btc\": " << p.inventory << ",\n"
        << "  \"limitations\": \"Unhedged inventory marked at final mid; no enforced exit. Partial fills use participation, without queue simulation. Spot source is indicated above; price-only S events are proxies, not observed midpoints. Funding mark is a minute-open proxy. Fee and filters are configurable assumptions, not account-verified.\"\n}\n";
    if (!fills || !quotes || !equity || !summary) throw std::runtime_error("Failed to write maker backtest results");
}
} // namespace btc::maker_backtest
