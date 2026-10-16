#include "paper-engine.h"
#include "../model/theo-model.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <istream>
#include <sstream>
#include <stdexcept>

namespace btc::paper {
namespace {
constexpr double bps = 10000.0;
constexpr Millis minute_ms = 60 * 1000;

std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> cells(1);
    for (char ch : line) {
        if (ch == ',') cells.emplace_back();
        else if (ch != '\r') cells.back() += ch;
    }
    return cells;
}
double number(const std::string& text) {
    std::size_t used = 0;
    const double value = std::stod(text, &used);
    if (used != text.size() || !std::isfinite(value)) throw std::invalid_argument(text);
    return value;
}
double basis(double perp, double spot) { return (perp / spot - 1) * bps; }
} // namespace

EventReader::EventReader(std::istream& in, std::string perp, std::string spot)
    : in_(in), perp_(std::move(perp)), spot_(std::move(spot)) {}

bool EventReader::next(Event& e) {
    std::string line;
    while (std::getline(in_, line)) {
        const auto c = split(line);
        if (c.size() < 11 || c[0] == "recv_ms") { if (!line.empty() && c[0] != "recv_ms") ++skipped_; continue; }
        if (c[3] != perp_ && c[3] != spot_) continue;
        try {
            e = Event{};
            e.time = std::stoll(c[0]);
            e.leg = c[3] == perp_ ? Perp : Spot;
            e.kind = c[2] == "T" ? 'T' : 'Q';
            if (c[2] == "Q") {
                e.bid = number(c[4]); e.bid_qty = number(c[5]); e.ask = number(c[6]); e.ask_qty = number(c[7]);
                if (!(e.bid > 0 && e.ask >= e.bid)) throw std::invalid_argument("crossed or empty quote");
            } else if (c[2] == "T") {
                e.price = number(c[8]); e.size = number(c[9]);
                if (!(e.price > 0 && e.size > 0)) throw std::invalid_argument("bad trade");
            } else throw std::invalid_argument("unknown kind");
            return true;
        } catch (const std::exception&) { ++skipped_; }
    }
    return false;
}

Engine::Engine(Config config) : config_(config), btc_(config.contracts * bip::contract_btc),
                                center_(config.center_bps), funding_(config.previous_funding_bps) {
    if (config.contracts < 1) throw std::runtime_error("contracts must be >= 1");
    if (config.latency_ms < 0) throw std::runtime_error("latency must be >= 0");
    if (config.threshold_bps <= 0) throw std::runtime_error("threshold must be positive");
}

bool Engine::ready() const {
    if (!book_[Perp].valid() || !book_[Spot].valid()) return false;
    if (config_.ema_halflife_min <= 0) return true;
    return center_seeded_ && now_ - center_start_ >= static_cast<Millis>(config_.ema_halflife_min * minute_ms);
}

double Engine::perp_fee(double price, double fee_bps) const {
    return config_.contracts * (bip::contract_btc * price * fee_bps / bps + config_.perp_fee_usd_per_contract);
}

std::optional<double> Engine::mid_basis_bps() const {
    if (!book_[Perp].valid() || !book_[Spot].valid()) return std::nullopt;
    return basis(mid(Perp), mid(Spot));
}

int Engine::position_contracts() const {
    if (config_.mode == Mode::Maker) return inventory_;
    return position_ ? position_->direction * config_.contracts : 0;
}

double Engine::unrealized_taker() const {
    if (!position_ || !book_[Perp].valid() || !book_[Spot].valid()) return 0;
    return position_->direction * btc_ * ((mid(Perp) - position_->entry_perp) - (mid(Spot) - position_->entry_spot));
}

Totals Engine::totals() const {
    Totals t = realized_;
    if (config_.mode == Mode::Maker) {
        t.price_pnl = cash_ + (inventory_ && book_[Perp].valid() ? inventory_ * bip::contract_btc * mid(Perp) : 0);
    } else if (position_) {
        t.price_pnl += unrealized_taker();
        t.funding_pnl += position_->funding;
        t.fees += position_->fees;
    }
    return t;
}

double Engine::max_drawdown() const {
    double peak = 0, worst = 0;
    for (const auto& p : equity_) { peak = std::max(peak, p.totals.net()); worst = std::max(worst, peak - p.totals.net()); }
    return worst;
}

void Engine::run_due(Millis time) {
    if (order_ && order_->due <= time) { const Order o = *order_; order_.reset(); execute(o); }
    for (auto& pending : pending_)
        if (pending && pending->due <= time) {
            live_[&pending - pending_] = pending->quote;
            pending.reset();
        }
}

void Engine::advance_to(Millis time) {
    if (now_ < 0) {
        now_ = time;
        next_equity_ = (time / minute_ms + 1) * minute_ms;
        funding_.advance(time);
        return;
    }
    time = std::max(time, now_); // Never let a clock blip move us backwards.
    run_due(time);               // Orders that arrived before this event see the prior book.
    if (const auto rate = funding_.advance(time); rate && position_contracts() && book_[Perp].valid()) {
        const double paid = bip::payment_usd(position_contracts(), mid(Perp), *rate);
        if (position_) { position_->funding -= paid; ++position_->funding_payments; }
        else realized_.funding_pnl -= paid;
    }
    for (; next_equity_ <= time; next_equity_ += minute_ms)
        equity_.push_back({next_equity_, totals(), position_contracts()});
    now_ = time;
}

void Engine::on_event(const Event& e, bool decide_now) {
    advance_to(e.time);
    const auto leg = e.leg == Perp ? bip::FundingEstimator::Futures : bip::FundingEstimator::Spot;
    if (e.kind == 'Q') {
        book_[e.leg] = {e.bid, e.bid_qty, e.ask, e.ask_qty};
        funding_.on_quote(leg, now_, e.bid, e.ask);
    } else {
        funding_.on_trade(leg, now_, e.price, e.size);
        if (e.leg == Perp && config_.mode == Mode::Maker) maker_trade(e.price);
    }
    update_center();
    if (decide_now) { decide(); run_due(now_); } // run_due here handles zero latency.
}

void Engine::update_center() {
    if (config_.ema_halflife_min <= 0) return;
    const auto b = mid_basis_bps();
    if (!b) return;
    if (!center_seeded_) { center_ = *b; center_seeded_ = true; center_start_ = center_time_ = now_; return; }
    const double dt = static_cast<double>(now_ - center_time_);
    center_ += (1 - std::exp(-dt * std::log(2.0) / (config_.ema_halflife_min * minute_ms))) * (*b - center_);
    center_time_ = now_;
}

void Engine::decide() {
    if (config_.mode == Mode::Taker) decide_taker(); else decide_maker();
}

void Engine::decide_taker() {
    if (!ready() || order_) return;
    const auto& p = book_[Perp]; const auto& s = book_[Spot];
    const double sell = basis(p.bid, s.ask), buy = basis(p.ask, s.bid); // Executable now.
    const double thr = config_.threshold_bps;
    if (!position_) {
        if (sell - center_ > thr) order_ = Order{now_ + config_.latency_ms, false, -1, sell, ""};
        else if (buy - center_ < -thr) order_ = Order{now_ + config_.latency_ms, false, +1, buy, ""};
        return;
    }
    const auto& pos = *position_;
    const int d = pos.direction;
    const double perp_exit = d > 0 ? p.bid : p.ask, spot_exit = d > 0 ? s.ask : s.bid;
    const double exit_fees = perp_fee(perp_exit, config_.perp_taker_bps) + btc_ * spot_exit * config_.spot_taker_bps / bps;
    const double pnl = d * btc_ * ((perp_exit - pos.entry_perp) - (spot_exit - pos.entry_spot)) + pos.funding - pos.fees - exit_fees;
    const double exit_basis = (d > 0 ? sell : buy) - center_;
    const char* reason = nullptr;
    if (config_.exit_on_profit && pnl > 0) reason = "PROFIT";
    else if (d > 0 ? exit_basis >= -thr : exit_basis <= thr) reason = "CONVERGED";
    else if (now_ - pos.entry >= config_.max_hold_ms) reason = "TIMEOUT";
    if (reason) order_ = Order{now_ + config_.latency_ms, true, d, exit_basis + center_, reason};
}

void Engine::execute(const Order& o) {
    if (!book_[Perp].valid() || !book_[Spot].valid()) return;
    // Fills are stamped at the order's arrival; the book is the one in force then.
    const auto& p = book_[Perp]; const auto& s = book_[Spot];
    // Entry long perp buys the perp ask and sells the spot bid; exits reverse it.
    const int buy_perp = o.exit ? -o.direction : o.direction;
    const double perp_px = buy_perp > 0 ? p.ask : p.bid, spot_px = buy_perp > 0 ? s.bid : s.ask;
    const double perp_cost = perp_fee(perp_px, config_.perp_taker_bps);
    const double spot_cost = btc_ * spot_px * config_.spot_taker_bps / bps;
    const double fill_basis = basis(perp_px, spot_px);
    const int inventory_after = o.exit ? 0 : o.direction * config_.contracts;
    fills_.push_back({o.due, Perp, buy_perp, perp_px, btc_, perp_cost, inventory_after, fill_basis});
    fills_.push_back({o.due, Spot, -buy_perp, spot_px, btc_, spot_cost, inventory_after, fill_basis});
    if (!o.exit) {
        position_ = Position{o.direction, o.due, perp_px, spot_px, o.signal_basis, fill_basis, perp_cost + spot_cost, 0, 0};
        return;
    }
    const auto pos = *position_;
    position_.reset();
    RoundTrip t;
    t.entry = pos.entry; t.exit = o.due; t.direction = pos.direction;
    t.entry_spot = pos.entry_spot; t.entry_perp = pos.entry_perp; t.exit_spot = spot_px; t.exit_perp = perp_px;
    t.signal_basis_bps = pos.signal_basis; t.entry_basis_bps = pos.entry_basis; t.exit_basis_bps = fill_basis;
    t.price_pnl = pos.direction * btc_ * ((perp_px - pos.entry_perp) - (spot_px - pos.entry_spot));
    t.funding_pnl = pos.funding; t.fees = pos.fees + perp_cost + spot_cost;
    t.net_pnl = t.price_pnl + t.funding_pnl - t.fees;
    t.net_bps = t.net_pnl / (btc_ * pos.entry_spot) * bps;
    t.funding_payments = pos.funding_payments;
    t.forced_end = o.reason == "END";
    t.exit_reason = o.reason;
    realized_.price_pnl += t.price_pnl; realized_.funding_pnl += t.funding_pnl; realized_.fees += t.fees;
    trips_.push_back(t);
}

void Engine::decide_maker() {
    Quote target[2];
    if (ready()) {
        const double spot = mid(Spot);
        const double contract_usd = bip::contract_btc * mid(Perp);
        theo::Inputs in;
        in.spot = spot;
        in.expected_exit_basis_bps = center_;
        Millis to_hour = bip::hour_ms - ((now_ % bip::hour_ms) + bip::hour_ms) % bip::hour_ms;
        in.expected_funding_paid_bps = theo::projected_hourly_funding_bps(
            funding_.projected_rate_bps(), config_.horizon_min, to_hour / 60000.0);
        in.inventory_btc = inventory_ * bip::contract_btc;
        in.perp_bid = book_[Perp].bid; in.perp_ask = book_[Perp].ask;
        theo::Config qc;
        // Two perp maker fills per bid/ask cycle; no hedge legs until cop and lean.
        qc.round_trip_fee_bps = 2 * (config_.perp_maker_bps + config_.perp_fee_usd_per_contract / contract_usd * bps);
        qc.round_trip_profit_bps = config_.profit_bps;
        qc.inventory_skew_bps_per_btc = config_.skew_bps_per_btc;
        qc.quote_size_btc = btc_;
        qc.max_inventory_btc = config_.max_inventory_contracts * bip::contract_btc;
        qc.price_tick = bip::price_tick;
        qc.size_increment_btc = bip::contract_btc;
        qc.min_order_notional = 0;
        try {
            const auto q = theo::price(in, qc);
            const double size = btc_ - 1e-9;
            if (q.bid_size_btc >= size) target[0] = {true, q.bid};
            if (q.ask_size_btc >= size) target[1] = {true, q.ask};
        } catch (const std::runtime_error&) {} // Locked/odd book: pull quotes until it clears.
    }
    for (int side = 0; side < 2; ++side) {
        const Quote& intent = pending_[side] ? pending_[side]->quote : live_[side];
        const bool same = intent.active == target[side].active && (!intent.active || intent.price == target[side].price);
        // A replace reaches the exchange after the latency; until then the old quote stays live.
        if (!same) pending_[side] = PendingQuote{now_ + config_.latency_ms, target[side]};
    }
}

void Engine::maker_trade(double price) {
    for (int side = 0; side < 2; ++side) {
        auto& q = live_[side];
        if (!q.active) continue;
        const bool hit = side == 0 ? (price < q.price || (config_.fill_at_touch && price == q.price))
                                   : (price > q.price || (config_.fill_at_touch && price == q.price));
        if (!hit) continue;
        const int dir = side == 0 ? +1 : -1;
        const double fee = perp_fee(q.price, config_.perp_maker_bps);
        inventory_ += dir * config_.contracts;
        cash_ -= dir * btc_ * q.price;
        realized_.fees += fee;
        fills_.push_back({now_, Perp, dir, q.price, btc_, fee, inventory_,
                          book_[Spot].valid() ? basis(q.price, mid(Spot)) : 0});
        q.active = false; // Filled quotes are gone; decide() re-quotes after the latency.
    }
}

void Engine::finish() {
    if (now_ < 0) return;
    if (config_.mode == Mode::Taker) {
        if (order_ && !order_->exit) order_.reset(); // An entry still in flight never fills.
        if (position_) { order_.reset(); execute(Order{now_, true, position_->direction, 0, "END"}); }
    }
    equity_.push_back({now_, totals(), position_contracts()});
}

std::string utc(Millis ms) {
    const std::time_t seconds = static_cast<std::time_t>(ms / 1000);
    std::ostringstream out;
    out << std::put_time(std::gmtime(&seconds), "%Y-%m-%dT%H:%M:%S") << '.'
        << std::setw(3) << std::setfill('0') << (ms % 1000) << 'Z';
    return out.str();
}

void write_results(const Engine& engine, const std::string& directory, const std::string& source) {
    std::filesystem::create_directories(directory);
    const auto& c = engine.config();
    const bool maker = c.mode == Mode::Maker;
    std::ofstream trades(directory + "/trades.csv"), fills(directory + "/fills.csv"), equity(directory + "/equity.csv");
    for (auto* f : {&trades, &fills, &equity}) *f << std::fixed << std::setprecision(10);
    trades << "entry_ts,exit_ts,entry_ms,exit_ms,entry_utc,exit_utc,side,quantity_btc,entry_spot,entry_perp,exit_spot,exit_perp,"
              "signal_basis_bps,entry_basis_bps,exit_basis_bps,price_pnl,funding_pnl,fees,net_pnl,net_bps,"
              "funding_payments,forced_end,exit_reason\n";
    int wins = 0, longs = 0;
    double net_bps = 0, slippage = 0;
    for (const auto& t : engine.trips()) {
        trades << t.entry / 1000 << ',' << t.exit / 1000 << ',' << t.entry << ',' << t.exit << ',' << utc(t.entry) << ','
               << utc(t.exit) << ',' << (t.direction > 0 ? "LONG_PERP_SHORT_SPOT" : "SHORT_PERP_LONG_SPOT") << ','
               << c.contracts * bip::contract_btc << ',' << t.entry_spot << ',' << t.entry_perp << ',' << t.exit_spot << ','
               << t.exit_perp << ',' << t.signal_basis_bps << ',' << t.entry_basis_bps << ',' << t.exit_basis_bps << ','
               << t.price_pnl << ',' << t.funding_pnl << ',' << t.fees << ',' << t.net_pnl << ',' << t.net_bps << ','
               << t.funding_payments << ',' << t.forced_end << ',' << t.exit_reason << '\n';
        wins += t.net_pnl > 0; longs += t.direction > 0; net_bps += t.net_bps;
        // Positive = the fill was worse than the basis that triggered the order.
        slippage += t.direction > 0 ? t.entry_basis_bps - t.signal_basis_bps : t.signal_basis_bps - t.entry_basis_bps;
    }
    fills << "ms,utc,leg,side,price,btc,fee,inventory_contracts,basis_bps\n";
    for (const auto& f : engine.fills())
        fills << f.time << ',' << utc(f.time) << ',' << (f.leg == Perp ? "PERP" : "SPOT") << ',' << (f.side > 0 ? "BUY" : "SELL")
              << ',' << f.price << ',' << f.btc << ',' << f.fee << ',' << f.inventory << ',' << f.basis_bps << '\n';
    equity << "ts,utc,price_pnl,funding_pnl,fees,net_pnl,direction\n";
    for (const auto& p : engine.equity())
        equity << p.time / 1000 << ',' << utc(p.time) << ',' << p.totals.price_pnl << ',' << p.totals.funding_pnl << ','
               << p.totals.fees << ',' << p.totals.net() << ',' << p.position << '\n';
    const auto n = engine.trips().size();
    const auto total = engine.totals();
    const auto& eq = engine.equity();
    std::ofstream summary(directory + "/summary.json");
    summary << std::fixed << std::setprecision(6) << "{\n"
            << "  \"source\": \"" << source << "\",\n"
            << "  \"mode\": \"" << (maker ? "maker" : "taker") << "\",\n"
            << "  \"start_utc\": \"" << (eq.empty() ? "" : utc(eq.front().time)) << "\",\n"
            << "  \"end_utc\": \"" << (eq.empty() ? "" : utc(eq.back().time)) << "\",\n"
            << "  \"latency_ms\": " << c.latency_ms << ",\n"
            << "  \"threshold_bps\": " << c.threshold_bps << ",\n"
            << "  \"maker_profit_bps\": " << c.profit_bps << ", \"maker_skew_bps_per_btc\": " << c.skew_bps_per_btc
            << ", \"maker_horizon_min\": " << c.horizon_min << ", \"fill_at_touch\": " << c.fill_at_touch << ",\n"
            << "  \"center_bps\": " << c.center_bps << ",\n"
            << "  \"ema_halflife_min\": " << c.ema_halflife_min << ",\n"
            << "  \"contracts\": " << c.contracts << ",\n"
            << "  \"holding_minutes\": " << c.max_hold_ms / 60000.0 << ",\n"
            << "  \"perp_taker_bps\": " << c.perp_taker_bps << ", \"perp_maker_bps\": " << c.perp_maker_bps
            << ", \"perp_fee_usd_per_contract\": " << c.perp_fee_usd_per_contract << ", \"spot_taker_bps\": " << c.spot_taker_bps << ",\n"
            << "  \"trades\": " << n << ",\n"
            << "  \"winning_trades\": " << wins << ",\n"
            << "  \"long_basis_trades\": " << longs << ",\n"
            << "  \"short_basis_trades\": " << n - longs << ",\n"
            << "  \"fills\": " << engine.fills().size() << ",\n"
            << "  \"mean_trade_bps\": " << (n ? net_bps / n : 0) << ",\n"
            << "  \"mean_entry_slippage_bps\": " << (n ? slippage / n : 0) << ",\n"
            << "  \"price_pnl\": " << total.price_pnl << ",\n"
            << "  \"funding_pnl\": " << total.funding_pnl << ",\n"
            << "  \"fees\": " << total.fees << ",\n"
            << "  \"net_pnl\": " << total.net() << ",\n"
            << "  \"max_drawdown\": " << engine.max_drawdown() << ",\n"
            << "  \"final_position_contracts\": " << engine.position_contracts() << "\n}\n";
}
} // namespace btc::paper
