#include "basis-maker.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <stdexcept>

namespace btc::basis_maker {
namespace {
constexpr double bps = 10000;
constexpr Millis default_interval_ms = 8*3600000;
}
Engine::Engine(Config config) : config_(config) {
    for (double v : {config.order_btc,config.max_gross_perp_btc,config.quantity_step,config.tick,
                    config.perp_min_notional,config.spot_min_notional,config.participation})
        if (!std::isfinite(v) || v <= 0) throw std::runtime_error("Basis sizes, tick and participation must be positive and finite");
    for (double v : {config.perp_maker_bps,config.spot_taker_bps,config.proxy_spot_half_spread_bps})
        if (!std::isfinite(v) || v < 0 || v >= bps) throw std::runtime_error("Basis fees and proxy spread must be in [0,10000) bps");
    if (!std::isfinite(config.equilibrium_bps) || config.equilibrium_bps <= -bps || config.participation > 1 ||
        config.funding_cancel_ms < 0 || config.funding_cancel_ms >= default_interval_ms ||
        config.max_spot_age_ms < 0 || config.max_perp_age_ms < 0 ||
        quantity(std::min(config.order_btc,config.max_gross_perp_btc)) < config.quantity_step)
        throw std::runtime_error("Invalid basis equilibrium, cancellation window, age or quantity");
}
double Engine::quantity(double size) const {
    return std::floor(std::max(0.0,size)/config_.quantity_step+1e-9)*config_.quantity_step;
}
bool Engine::ready() const {
    return spot_bid_ > 0 && spot_ask_ >= spot_bid_ && perp_bid_ > 0 && perp_ask_ >= perp_bid_ &&
        now_-spot_time_ <= config_.max_spot_age_ms && now_-perp_time_ <= config_.max_perp_age_ms;
}
double Engine::basis() const {
    return ((perp_bid_+perp_ask_)/(spot_bid_+spot_ask_)-1)*bps;
}
bool Engine::funding_guard() const {
    // Missing a new schedule event after its expected settlement fails closed.
    return next_funding_ < 0 || now_ >= next_funding_-config_.funding_cancel_ms;
}
Totals Engine::totals() const {
    Totals t;
    t.price_pnl = cash_+perp_btc_*(perp_bid_+perp_ask_)/2+spot_btc_*(spot_bid_+spot_ask_)/2;
    t.fees = fees_; t.perp_btc = perp_btc_; t.spot_btc = spot_btc_; t.gross_perp_btc = gross_btc_;
    return t; // The user explicitly requested zero funding cash flows and no exits.
}
void Engine::decide() {
    int side = 0;
    double price = 0, size = 0, observed_basis = 0;
    std::string reason = "MISSING_OR_STALE_MARKET";
    if (ready()) {
        observed_basis = basis();
        if (funding_guard()) reason = "PRE_FUNDING_CANCEL";
        else if (quantity(config_.max_gross_perp_btc-gross_btc_) < config_.quantity_step) reason = "GROSS_POSITION_CAP";
        else {
            if (observed_basis < config_.equilibrium_bps-1e-9) { side = 1; price = perp_bid_; }
            else if (observed_basis > config_.equilibrium_bps+1e-9) { side = -1; price = perp_ask_; }
            else reason = "AT_EQUILIBRIUM";
            if (side) {
                price = (side > 0 ? std::floor(price/config_.tick+1e-9) : std::ceil(price/config_.tick-1e-9))*config_.tick;
                size = quantity(std::min(config_.order_btc,config_.max_gross_perp_btc-gross_btc_));
                const double hedge_price = side > 0 ? spot_bid_ : spot_ask_;
                if (size*price < config_.perp_min_notional || size*hedge_price < config_.spot_min_notional) {
                    reason = "MIN_NOTIONAL"; side = 0; size = 0;
                } else if (side > 0 ? price >= perp_ask_-config_.tick*1e-7 : price <= perp_bid_+config_.tick*1e-7) {
                    reason = "POST_ONLY_REJECT"; side = 0; size = 0;
                } else reason = side > 0 ? "BID_PERP_SHORT_SPOT_ON_FILL" : "OFFER_PERP_LONG_SPOT_ON_FILL";
            }
        }
    }
    if (quote_side_ != side || (side && std::abs(price-quote_price_) > config_.tick*1e-7) || remaining_ < config_.quantity_step) {
        quote_side_ = side; quote_price_ = price; remaining_ = side ? size : 0; quote_since_ = now_;
    }
    if (decisions_.empty() || decisions_.back().reason != reason || decisions_.back().side != side ||
        std::abs(decisions_.back().price-price) > config_.tick*1e-7) {
        decisions_.push_back({now_,reason,side,observed_basis,price,remaining_});
    }
}
void Engine::trade(const Event& e) {
    if (!ready() || funding_guard() || !quote_side_ || e.time <= quote_since_) return;
    const double observed_basis = basis();
    if (quote_side_ > 0 ? observed_basis >= config_.equilibrium_bps-1e-9 : observed_basis <= config_.equilibrium_bps+1e-9) return;
    if (e.buyer_maker != (quote_side_ > 0)) return;
    const double gap = quote_side_ > 0 ? quote_price_-e.price : e.price-quote_price_;
    const double tolerance = config_.tick*1e-7;
    if (gap <= tolerance && !(config_.fill_at_touch && std::abs(gap) <= tolerance)) return;
    const double q = quantity(std::min({remaining_,e.quantity*config_.participation,config_.max_gross_perp_btc-gross_btc_}));
    if (q <= 0) return;
    const int direction = quote_side_;
    const double perp_price = quote_price_, spot_price = direction > 0 ? spot_bid_ : spot_ask_;
    if (q*spot_price < config_.spot_min_notional) return; // Do not create a fill whose immediate hedge is too small.
    const double perp_fee = q*perp_price*config_.perp_maker_bps/bps;
    const double spot_fee = q*spot_price*config_.spot_taker_bps/bps;
    const std::size_t id = pairs_.size()+1;
    // Equal BTC is hedged immediately, including every partial perp fill.
    // Principal notionals differ by q*(perp_price-spot_price), as requested
    // after the user clarified that BTC, rather than dollars, should match.
    perp_btc_ += direction*q; spot_btc_ -= direction*q;
    gross_btc_ += q;
    cash_ += direction*q*(spot_price-perp_price);
    fees_ += perp_fee+spot_fee;
    remaining_ = quantity(remaining_-q);
    fills_.push_back({now_,id,"PERP","MAKER",direction,perp_price,q,q*perp_price,perp_fee});
    fills_.push_back({now_,id,"SPOT","TAKER",-direction,spot_price,q,q*spot_price,spot_fee});
    pairs_.push_back({id,now_,direction,q,perp_price,spot_price,(perp_price/spot_price-1)*bps,perp_fee+spot_fee});
}
void Engine::on_event(const Event& e, bool) {
    if (e.time < 0 || e.time < now_) throw std::runtime_error("Basis events must be chronological");
    for (double v : {e.price,e.quantity,e.bid,e.ask,e.funding_rate,e.interval_hours})
        if (!std::isfinite(v)) throw std::runtime_error("Basis event values must be finite");
    now_ = e.time;
    if (next_funding_ < 0) next_funding_ = (now_/default_interval_ms+1)*default_interval_ms;
    if (e.kind == 'S') {
        if (e.bid > 0 && e.ask >= e.bid) { spot_bid_ = e.bid; spot_ask_ = e.ask; ++mid_events_; }
        else {
            if (e.bid != 0 || e.ask != 0 || e.price <= 0) throw std::runtime_error("Invalid basis spot reference");
            const double half = config_.proxy_spot_half_spread_bps/bps;
            spot_bid_ = e.price*(1-half); spot_ask_ = e.price*(1+half); ++proxy_events_;
        }
        spot_time_ = now_;
    } else if (e.kind == 'Q') {
        if (e.bid <= 0 || e.ask < e.bid) throw std::runtime_error("Invalid basis perp book");
        perp_bid_ = e.bid; perp_ask_ = e.ask; perp_time_ = now_;
    } else if (e.kind == 'T') {
        if (e.price <= 0 || e.quantity <= 0) throw std::runtime_error("Invalid basis perp trade");
        trade(e);
    } else if (e.kind == 'F' || e.kind == 'R') {
        if (e.interval_hours <= 0 || e.interval_hours > 24) throw std::runtime_error("Invalid basis funding interval");
        next_funding_ = now_+static_cast<Millis>(e.interval_hours*3600000);
        // Funding payments are intentionally zero in this requested scenario.
    } else throw std::runtime_error("Unknown basis event");
    decide(); // Zero latency: references and entry cancellations apply immediately.
    const auto t = totals();
    peak_ = std::max(peak_,t.net()); drawdown_ = std::max(drawdown_,peak_-t.net());
    if (ready() && (last_equity_ < 0 || now_/60000 > last_equity_/60000)) {
        equity_.push_back({now_,t}); last_equity_ = now_;
    }
}
void Engine::finish() {
    quote_side_ = 0; remaining_ = 0;
    if (now_ >= 0) equity_.push_back({now_,totals()}); // No exit or artificial closing fill.
}
void replay(Engine& engine, const std::string& path) {
    maker_backtest::read_events(path,[&](const Event& e,bool decide) { engine.on_event(e,decide); });
    engine.finish();
}
void write_results(const Engine& engine, const std::string& directory) {
    std::filesystem::create_directories(directory);
    std::ofstream fills(directory+"/fills.csv"),pairs(directory+"/pairs.csv"),quotes(directory+"/quotes.csv"),equity(directory+"/equity.csv");
    for (auto* stream : {&fills,&pairs,&quotes,&equity}) *stream << std::fixed << std::setprecision(10);
    fills << "time_ms,pair_id,leg,liquidity,side,price,btc,notional,fee\n";
    for (const auto& f : engine.fills()) fills << f.time << ',' << f.pair_id << ',' << f.leg << ',' << f.liquidity << ','
        << (f.side > 0 ? "BUY" : "SELL") << ',' << f.price << ',' << f.btc << ',' << f.notional << ',' << f.fee << '\n';
    pairs << "pair_id,time_ms,direction,perp_btc,spot_btc,perp_price,spot_price,perp_notional,spot_notional,notional_difference,entry_basis_bps,entry_fees\n";
    for (const auto& p : engine.pairs()) pairs << p.id << ',' << p.time << ',' << (p.direction > 0 ? "LONG_PERP_SHORT_SPOT" : "SHORT_PERP_LONG_SPOT") << ','
        << p.btc << ',' << p.btc << ',' << p.perp_price << ',' << p.spot_price << ',' << p.btc*p.perp_price << ',' << p.btc*p.spot_price << ','
        << p.btc*(p.perp_price-p.spot_price) << ',' << p.entry_basis_bps << ',' << p.fees << '\n';
    quotes << "time_ms,status,side,perp_mid_basis_bps,price,remaining_btc\n";
    for (const auto& q : engine.decisions()) quotes << q.time << ',' << q.reason << ',' << q.side << ',' << q.perp_mid_basis_bps << ',' << q.price << ',' << q.remaining << '\n';
    equity << "time_ms,price_pnl,fees,funding_pnl,net_pnl,perp_btc,spot_btc,delta_btc,gross_perp_btc\n";
    for (const auto& e : engine.equity()) equity << e.time << ',' << e.totals.price_pnl << ',' << e.totals.fees << ',' << e.totals.funding_pnl << ','
        << e.totals.net() << ',' << e.totals.perp_btc << ',' << e.totals.spot_btc << ',' << e.totals.perp_btc+e.totals.spot_btc << ',' << e.totals.gross_perp_btc << '\n';
    const auto& c = engine.config(); const auto t = engine.totals();
    std::ofstream summary(directory+"/summary.json");
    summary << std::fixed << std::setprecision(10) << "{\n"
        << "  \"strategy\": \"passive perp basis entry, immediate equal-BTC spot hedge\",\n"
        << "  \"equilibrium_bps\": " << c.equilibrium_bps << ",\n  \"latency_ms\": 0,\n  \"exits_enabled\": false,\n"
        << "  \"funding_assumption\": \"zero funding cash flows\",\n  \"funding_cancel_seconds\": " << c.funding_cancel_ms/1000.0 << ",\n"
        << "  \"perp_maker_bps\": " << c.perp_maker_bps << ",\n  \"spot_taker_bps\": " << c.spot_taker_bps << ",\n"
        << "  \"order_btc\": " << c.order_btc << ",\n  \"max_gross_perp_btc\": " << c.max_gross_perp_btc << ",\n"
        << "  \"participation\": " << c.participation << ",\n  \"fill_at_touch\": " << (c.fill_at_touch ? "true" : "false") << ",\n"
        << "  \"proxy_spot_half_spread_bps\": " << c.proxy_spot_half_spread_bps << ",\n  \"spot_proxy_events\": " << engine.spot_proxy_events() << ",\n"
        << "  \"spot_mid_events\": " << engine.spot_mid_events() << ",\n  \"paired_partial_fills\": " << engine.pairs().size() << ",\n"
        << "  \"leg_fills\": " << engine.fills().size() << ",\n  \"price_pnl\": " << t.price_pnl << ",\n  \"fees\": " << t.fees << ",\n"
        << "  \"funding_pnl\": 0,\n  \"net_pnl\": " << t.net() << ",\n  \"perp_btc\": " << t.perp_btc << ",\n  \"spot_btc\": " << t.spot_btc << ",\n"
        << "  \"delta_btc\": " << t.perp_btc+t.spot_btc << ",\n  \"gross_perp_btc\": " << t.gross_perp_btc << ",\n  \"max_drawdown\": " << engine.max_drawdown() << ",\n"
        << "  \"limitations\": \"Open positions marked at mids, not realized returns. Spot price-only input uses last-trade proxy with assumed spread. Borrow availability, borrow interest and spot impact are not modeled. Opposite entries can offset net positions; gross entry capacity is not recycled. No queue reconstruction.\"\n}\n";
    if (!fills || !pairs || !quotes || !equity || !summary) throw std::runtime_error("Failed writing paired basis results");
}
} // namespace btc::basis_maker
