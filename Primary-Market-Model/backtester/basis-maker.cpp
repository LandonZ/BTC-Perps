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
    for (double v : {config.order_btc,config.quantity_step,config.tick,
                    config.perp_min_notional,config.spot_min_notional,config.participation})
        if (!std::isfinite(v) || v <= 0) throw std::runtime_error("Basis sizes, tick and participation must be positive and finite");
    if (!std::isfinite(config.max_gross_perp_btc) || config.max_gross_perp_btc < 0)
        throw std::runtime_error("Position cap must be nonnegative; zero means unlimited");
    for (double v : {config.perp_maker_bps,config.perp_taker_bps,config.spot_taker_bps,config.proxy_spot_half_spread_bps})
        if (!std::isfinite(v) || v < 0 || v >= bps) throw std::runtime_error("Basis fees and proxy spread must be in [0,10000) bps");
    if (!std::isfinite(config.funding_interest_bps) || !std::isfinite(config.funding_clamp_bps) || config.funding_clamp_bps <= 0)
        throw std::runtime_error("Funding interest and clamp must be finite, with positive clamp");
    if (!std::isfinite(config.equilibrium_bps) || config.equilibrium_bps <= -bps || config.participation > 1 ||
        config.funding_cancel_ms < 0 || config.funding_cancel_ms >= default_interval_ms ||
        config.max_spot_age_ms < 0 || config.max_perp_age_ms < 0 ||
        quantity(config.max_gross_perp_btc > 0 ? std::min(config.order_btc,config.max_gross_perp_btc) : config.order_btc) < config.quantity_step)
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
    t.open_gross_perp_btc = open_gross_btc_; t.realized_pnl = realized_pnl_;
    t.funding_pnl = funding_pnl_;
    return t;
}
double Engine::used_capacity() const {
    return (config_.exit_on_equilibrium || config_.exit_before_funding) ? open_gross_btc_ : gross_btc_;
}
double Engine::available_capacity() const {
    return config_.max_gross_perp_btc > 0 ? config_.max_gross_perp_btc-used_capacity() : config_.order_btc;
}
void Engine::close_converged() {
    if (!config_.exit_on_equilibrium || open_pairs_.empty() || !ready()) return;
    const double observed_basis = basis();
    // Every live pair has the same direction: crossing the shared equilibrium
    // closes that side before opposite entries become eligible. Avoid scanning
    // all live lots on every tick when no crossing occurred.
    const int direction = pairs_[open_pairs_.front()].direction;
    if (direction > 0 ? observed_basis < config_.equilibrium_bps-1e-9
                      : observed_basis > config_.equilibrium_bps+1e-9) return;
    auto keep = open_pairs_.begin();
    for (auto index : open_pairs_) {
        auto& p = pairs_[index];
        const bool converged = p.direction > 0 ? observed_basis >= config_.equilibrium_bps-1e-9
                                               : observed_basis <= config_.equilibrium_bps+1e-9;
        if (!converged) { *keep++ = index; continue; }
        close_pair(p,"EQUILIBRIUM");
    }
    open_pairs_.erase(keep,open_pairs_.end());
    if (direction > 0) open_long_btc_ = 0; else open_short_btc_ = 0;
    if (std::abs(open_gross_btc_) < 1e-10) open_gross_btc_ = 0;
    if (std::abs(perp_btc_) < 1e-10) perp_btc_ = 0;
    if (std::abs(spot_btc_) < 1e-10) spot_btc_ = 0;
}
void Engine::close_pair(Pair& p, const std::string& reason) {
    // Both reduce-only legs cross the current executable side, even in the
    // pre-funding entry blackout. A midpoint crossing is not a free fill.
    const double perp_price = p.direction > 0 ? perp_bid_ : perp_ask_;
    const double spot_price = p.direction > 0 ? spot_ask_ : spot_bid_;
    const double perp_fee = p.btc*perp_price*config_.perp_taker_bps/bps;
    const double spot_fee = p.btc*spot_price*config_.spot_taker_bps/bps;
    fills_.push_back({now_,p.id,"PERP","TAKER",-p.direction,perp_price,p.btc,p.btc*perp_price,perp_fee,"EXIT"});
    fills_.push_back({now_,p.id,"SPOT","TAKER",p.direction,spot_price,p.btc,p.btc*spot_price,spot_fee,"EXIT"});
    cash_ += p.direction*p.btc*(perp_price-spot_price);
    perp_btc_ -= p.direction*p.btc; spot_btc_ += p.direction*p.btc;
    open_gross_btc_ -= p.btc;
    (p.direction > 0 ? open_long_btc_ : open_short_btc_) -= p.btc;
    fees_ += perp_fee+spot_fee;
    p.exit_reason = reason;
    p.closed = true; p.exit_time = now_; p.exit_perp_price = perp_price;
    p.exit_spot_price = spot_price; p.exit_fees = perp_fee+spot_fee;
    p.realized_pnl = p.direction*p.btc*((perp_price-p.perp_price)-(spot_price-p.spot_price))-p.fees-p.exit_fees+p.funding_pnl;
    // Funding cash was realized when paid, even while this pair was open.
    realized_pnl_ += p.realized_pnl-p.funding_pnl;
    quote_side_ = 0; remaining_ = 0;
}
void Engine::close_before_funding() {
    // Causal estimate from completed official premium-index minutes only.
    if (!config_.exit_before_funding || open_pairs_.empty() || !ready() || next_funding_ < 0 ||
        now_ < next_funding_-config_.funding_cancel_ms || now_ >= next_funding_ || !premium_ready()) return;
    const double premium = average_premium_bps();
    const double interest = config_.funding_interest_bps, clamp = config_.funding_clamp_bps;
    if (premium >= interest-clamp-1e-9 && premium <= interest+clamp+1e-9) return;
    const double rate = estimated_funding_bps();
    if (std::abs(rate) < 1e-9) return;
    const int payer = rate > 0 ? 1 : -1;
    if (config_.funding_exit_payers_only && (payer > 0 ? open_long_btc_ : open_short_btc_) < 1e-10) return;
    auto keep = open_pairs_.begin();
    for (auto index : open_pairs_) {
        auto& p = pairs_[index];
        if (config_.funding_exit_payers_only && p.direction != payer) { *keep++ = index; continue; }
        close_pair(p,"PRE_FUNDING_LINEAR_REGION");
    }
    open_pairs_.erase(keep,open_pairs_.end());
    if (!config_.funding_exit_payers_only || payer > 0) open_long_btc_ = 0;
    if (!config_.funding_exit_payers_only || payer < 0) open_short_btc_ = 0;
    if (std::abs(open_gross_btc_) < 1e-9) open_gross_btc_ = 0;
    if (std::abs(perp_btc_) < 1e-9) perp_btc_ = 0;
    if (std::abs(spot_btc_) < 1e-9) spot_btc_ = 0;
}
bool Engine::premium_ready() const {
    return premium_complete_ && premium_weight_ > 0 && premium_time_ > premium_period_start_ && now_-premium_time_ <= 90000;
}
double Engine::average_premium_bps() const {
    return premium_weight_ > 0 ? premium_weighted_/premium_weight_ : 0;
}
double Engine::estimated_funding_bps() const {
    const double p = average_premium_bps();
    return p+std::clamp(config_.funding_interest_bps-p,-config_.funding_clamp_bps,config_.funding_clamp_bps);
}

void Engine::decide() {
    int side = 0;
    double price = 0, size = 0, observed_basis = 0;
    std::string reason = "MISSING_OR_STALE_MARKET";
    if (ready()) {
        observed_basis = basis();
        if (funding_guard()) reason = "PRE_FUNDING_CANCEL";
        else if (quantity(available_capacity()) < config_.quantity_step)
            reason = (config_.exit_on_equilibrium || config_.exit_before_funding) ? "OPEN_POSITION_CAP" : "GROSS_POSITION_CAP";
        else {
            if (observed_basis < config_.equilibrium_bps-1e-9) { side = 1; price = perp_bid_; }
            else if (observed_basis > config_.equilibrium_bps+1e-9) { side = -1; price = perp_ask_; }
            else reason = "AT_EQUILIBRIUM";
            if (side) {
                price = (side > 0 ? std::floor(price/config_.tick+1e-9) : std::ceil(price/config_.tick-1e-9))*config_.tick;
                size = quantity(std::min(config_.order_btc,available_capacity()));
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
    const double q = quantity(std::min({remaining_,e.quantity*config_.participation,available_capacity()}));
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
    gross_btc_ += q; open_gross_btc_ += q;
    (direction > 0 ? open_long_btc_ : open_short_btc_) += q;
    cash_ += direction*q*(spot_price-perp_price);
    fees_ += perp_fee+spot_fee;
    remaining_ = quantity(remaining_-q);
    fills_.push_back({now_,id,"PERP","MAKER",direction,perp_price,q,q*perp_price,perp_fee});
    fills_.push_back({now_,id,"SPOT","TAKER",-direction,spot_price,q,q*spot_price,spot_fee});
    pairs_.push_back({id,now_,direction,q,perp_price,spot_price,(perp_price/spot_price-1)*bps,perp_fee+spot_fee});
    open_pairs_.push_back(pairs_.size()-1);
}
void Engine::on_event(const Event& e, bool) {
    if (e.time < 0 || e.time < now_) throw std::runtime_error("Basis events must be chronological");
    for (double v : {e.price,e.quantity,e.bid,e.ask,e.funding_rate,e.interval_hours})
        if (!std::isfinite(v)) throw std::runtime_error("Basis event values must be finite");
    now_ = e.time;
    if (next_funding_ < 0) {
        next_funding_ = (now_/default_interval_ms+1)*default_interval_ms;
        premium_period_start_ = next_funding_-default_interval_ms; premium_time_ = premium_period_start_;
    }
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
    } else if (e.kind == 'P') {
        // A candle is available at close_time+1ms, never at its open time.
        // The final candle of the preceding period arrives after settlement
        // and is discarded when the new funding period has already started.
        if (e.time > premium_period_start_ && e.time <= next_funding_) {
            if (e.time != premium_time_+60000) premium_complete_ = false;
            const double weight = (e.time-premium_period_start_)/60000.0;
            premium_weighted_ += weight*e.price*bps; premium_weight_ += weight;
            premium_time_ = e.time;
        }
    } else if (e.kind == 'F' || e.kind == 'R') {
        if (e.interval_hours <= 0 || e.interval_hours > 24) throw std::runtime_error("Invalid basis funding interval");
        if (e.kind == 'F' && config_.funding_cashflows) {
            if (e.price <= 0) throw std::runtime_error("Funding settlement requires a positive mark");
            const double payment = -perp_btc_*e.price*e.funding_rate;
            funding_pnl_ += payment; realized_pnl_ += payment;
            for (auto index : open_pairs_) {
                auto& p = pairs_[index];
                p.funding_pnl -= p.direction*p.btc*e.price*e.funding_rate;
            }
        }
        // Archive calc_time can lag the official UTC settlement boundary by
        // milliseconds. Do not turn that lag into time to trade on the final
        // candle of the period, or misalign all subsequent premium minutes.
        const Millis boundary = now_-now_%60000;
        next_funding_ = boundary+static_cast<Millis>(e.interval_hours*3600000);
        premium_period_start_ = boundary; premium_time_ = boundary;
        premium_weighted_ = 0; premium_weight_ = 0; premium_complete_ = true;
    } else throw std::runtime_error("Unknown basis event");
    close_converged();
    close_before_funding();
    decide(); // Zero latency: references, exits and entry cancellations apply immediately.
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
    replay_files(engine,{path});
}
void replay_files(Engine& engine, const std::vector<std::string>& paths, const std::string& premium_file) {
    std::vector<Event> premiums;
    if (!premium_file.empty()) events::read(premium_file,[&](const Event& e,bool) {
        if (e.kind != 'P' || (!premiums.empty() && e.time <= premiums.back().time))
            throw std::runtime_error("Premium input must contain chronological P events");
        premiums.push_back(e);
    });
    std::size_t premium_index = 0;
    Millis latest = -1;
    for (std::size_t i = 0; i < paths.size(); ++i) {
        events::read(paths[i],[&](const Event& e,bool decide) {
            // Each daily file may contain a prior-settlement schedule seed.
            // Carry the live engine across days instead of rewinding its clock.
            if (i && e.kind == 'R' && e.time <= latest) return;
            if (latest < 0) while (premium_index < premiums.size() && premiums[premium_index].time <= e.time) ++premium_index;
            while (premium_index < premiums.size() && (premiums[premium_index].time < e.time ||
                   (premiums[premium_index].time == e.time && e.kind != 'F' && e.kind != 'R'))) {
                engine.on_event(premiums[premium_index++]);
            }
            engine.on_event(e,decide); latest = e.time;
        });
    }
    engine.finish();
}
void write_results(const Engine& engine, const std::string& directory, bool details) {
    std::filesystem::create_directories(directory);
    std::ofstream fills,pairs,quotes,equity(directory+"/equity.csv");
    if (details) { fills.open(directory+"/fills.csv"); pairs.open(directory+"/pairs.csv"); quotes.open(directory+"/quotes.csv"); }
    for (auto* stream : {&fills,&pairs,&quotes,&equity}) *stream << std::fixed << std::setprecision(10);
    if (details) {
    fills << "time_ms,pair_id,leg,liquidity,side,price,btc,notional,fee,phase\n";
    for (const auto& f : engine.fills()) fills << f.time << ',' << f.pair_id << ',' << f.leg << ',' << f.liquidity << ','
        << (f.side > 0 ? "BUY" : "SELL") << ',' << f.price << ',' << f.btc << ',' << f.notional << ',' << f.fee << ',' << f.phase << '\n';
    pairs << "pair_id,time_ms,direction,perp_btc,spot_btc,perp_price,spot_price,perp_notional,spot_notional,notional_difference,entry_basis_bps,entry_fees,status,exit_time_ms,exit_perp_price,exit_spot_price,exit_fees,realized_pnl,funding_pnl,exit_reason\n";
    for (const auto& p : engine.pairs()) pairs << p.id << ',' << p.time << ',' << (p.direction > 0 ? "LONG_PERP_SHORT_SPOT" : "SHORT_PERP_LONG_SPOT") << ','
        << p.btc << ',' << p.btc << ',' << p.perp_price << ',' << p.spot_price << ',' << p.btc*p.perp_price << ',' << p.btc*p.spot_price << ','
        << p.btc*(p.perp_price-p.spot_price) << ',' << p.entry_basis_bps << ',' << p.fees << ','
        << (p.closed ? "CLOSED" : "OPEN") << ',' << p.exit_time << ',' << p.exit_perp_price << ',' << p.exit_spot_price
        << ',' << p.exit_fees << ',' << p.realized_pnl << ',' << p.funding_pnl << ',' << p.exit_reason << '\n';
    quotes << "time_ms,status,side,perp_mid_basis_bps,price,remaining_btc\n";
    for (const auto& q : engine.decisions()) quotes << q.time << ',' << q.reason << ',' << q.side << ',' << q.perp_mid_basis_bps << ',' << q.price << ',' << q.remaining << '\n';
    }
    equity << "time_ms,price_pnl,fees,funding_pnl,net_pnl,perp_btc,spot_btc,delta_btc,gross_perp_btc,open_gross_perp_btc,realized_pnl,unrealized_pnl\n";
    for (const auto& e : engine.equity()) equity << e.time << ',' << e.totals.price_pnl << ',' << e.totals.fees << ',' << e.totals.funding_pnl << ','
        << e.totals.net() << ',' << e.totals.perp_btc << ',' << e.totals.spot_btc << ',' << e.totals.perp_btc+e.totals.spot_btc << ',' << e.totals.gross_perp_btc << ',' << e.totals.open_gross_perp_btc
        << ',' << e.totals.realized_pnl << ',' << e.totals.net()-e.totals.realized_pnl << '\n';
    const auto& c = engine.config(); const auto t = engine.totals();
    std::size_t closed = 0, wins = 0, losses = 0, open = 0, max_open = 0, funding_closed = 0, convergence_closed = 0;
    double open_gross = 0, max_open_gross = 0;
    for (const auto& p : engine.pairs()) if (p.closed) {
        ++closed; wins += p.realized_pnl > 1e-10; losses += p.realized_pnl < -1e-10;
        funding_closed += p.exit_reason == "PRE_FUNDING_LINEAR_REGION"; convergence_closed += p.exit_reason == "EQUILIBRIUM";
    }
    for (const auto& f : engine.fills()) if (f.leg == "PERP") {
        if (f.phase == "ENTRY") { ++open; open_gross += f.btc; }
        else { --open; open_gross -= f.btc; }
        max_open = std::max(max_open,open); max_open_gross = std::max(max_open_gross,open_gross);
    }
    std::ofstream summary(directory+"/summary.json");
    summary << std::fixed << std::setprecision(10) << "{\n"
        << "  \"strategy\": \"passive perp basis entry, immediate equal-BTC spot hedge\",\n"
        << "  \"equilibrium_bps\": " << c.equilibrium_bps << ",\n  \"latency_ms\": 0,\n  \"exits_enabled\": " << ((c.exit_on_equilibrium || c.exit_before_funding) ? "true" : "false") << ",\n"
        << "  \"exit_execution\": \"" << (c.exit_on_equilibrium ? "Equilibrium crossings and any enabled funding exits" : c.exit_before_funding ? "Funding payers only in linear premium region during final pre-settlement window" : "No exits") << "\",\n"
        << "  \"capacity_rule\": \"" << (c.max_gross_perp_btc == 0 ? "Unlimited concurrent entry capacity" : (c.exit_on_equilibrium || c.exit_before_funding) ? "Open gross perp BTC; exits release capacity" : "Cumulative gross perp BTC; capacity not recycled") << "\",\n"
        << "  \"funding_assumption\": \"" << (c.funding_cashflows ? "Recorded settlement cash flows on held positions" : "Zero funding cash flows") << "\",\n  \"funding_cancel_seconds\": " << c.funding_cancel_ms/1000.0 << ",\n"
        << "  \"perp_maker_bps\": " << c.perp_maker_bps << ",\n  \"perp_taker_bps\": " << c.perp_taker_bps << ",\n  \"spot_taker_bps\": " << c.spot_taker_bps << ",\n"
        << "  \"cycle_fee_bps\": " << c.perp_maker_bps+c.perp_taker_bps+2*c.spot_taker_bps << ",\n"
        << "  \"exit_on_equilibrium\": " << (c.exit_on_equilibrium ? "true" : "false") << ",\n"
        << "  \"exit_before_funding\": " << (c.exit_before_funding ? "true" : "false") << ",\n"
        << "  \"funding_exit_payers_only\": " << (c.funding_exit_payers_only ? "true" : "false") << ",\n"
        << "  \"funding_cashflows\": " << (c.funding_cashflows ? "true" : "false") << ",\n"
        << "  \"funding_interest_bps\": " << c.funding_interest_bps << ",\n  \"funding_clamp_bps\": " << c.funding_clamp_bps << ",\n"
        << "  \"funding_exit_pairs\": " << funding_closed << ",\n  \"convergence_exit_pairs\": " << convergence_closed << ",\n"
        << "  \"funding_signal\": \"Causal weighted average of completed 1m premium-index CLOSE proxies; not exact exchange forecast\",\n"
        << "  \"order_btc\": " << c.order_btc << ",\n  \"max_gross_perp_btc\": " << c.max_gross_perp_btc << ",\n"
        << "  \"participation\": " << c.participation << ",\n  \"fill_at_touch\": " << (c.fill_at_touch ? "true" : "false") << ",\n"
        << "  \"proxy_spot_half_spread_bps\": " << c.proxy_spot_half_spread_bps << ",\n  \"spot_proxy_events\": " << engine.spot_proxy_events() << ",\n"
        << "  \"spot_mid_events\": " << engine.spot_mid_events() << ",\n  \"paired_partial_fills\": " << engine.pairs().size() << ",\n"
        << "  \"leg_fills\": " << engine.fills().size() << ",\n  \"price_pnl\": " << t.price_pnl << ",\n  \"fees\": " << t.fees << ",\n"
        << "  \"closed_pairs\": " << closed << ",\n  \"open_pairs\": " << engine.pairs().size()-closed << ",\n"
        << "  \"winning_closed_pairs\": " << wins << ",\n  \"losing_closed_pairs\": " << losses << ",\n"
        << "  \"max_concurrent_pairs\": " << max_open << ",\n  \"max_open_gross_perp_btc\": " << max_open_gross << ",\n"
        << "  \"realized_pnl\": " << t.realized_pnl << ",\n  \"unrealized_pnl\": " << t.net()-t.realized_pnl << ",\n"
        << "  \"funding_pnl\": " << t.funding_pnl << ",\n  \"net_pnl\": " << t.net() << ",\n  \"perp_btc\": " << t.perp_btc << ",\n  \"spot_btc\": " << t.spot_btc << ",\n"
        << "  \"final_perp_mid\": " << engine.final_perp_mid() << ",\n  \"final_spot_mid\": " << engine.final_spot_mid() << ",\n"
        << "  \"delta_btc\": " << t.perp_btc+t.spot_btc << ",\n  \"gross_perp_btc\": " << t.gross_perp_btc << ",\n"
        << "  \"open_gross_perp_btc\": " << t.open_gross_perp_btc << ",\n  \"max_drawdown\": " << engine.max_drawdown() << ",\n"
        << "  \"limitations\": \"Open positions marked at mids; closed pairs use both executable exit sides and all four leg fees. Spot price-only input uses last-trade proxy with assumed spread. Borrow availability, interest, impact, depth, margin and liquidation are not modeled. No queue reconstruction. Midpoint convergence does not guarantee a positive executable round trip.\"\n}\n";
    if ((details && (!fills || !pairs || !quotes)) || !equity || !summary) throw std::runtime_error("Failed writing paired basis results");
}
} // namespace btc::basis_maker
