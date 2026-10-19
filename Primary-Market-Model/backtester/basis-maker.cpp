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
    if (!std::isfinite(config.equilibrium_bps) || !std::isfinite(config.entry_offset_bps) || config.entry_offset_bps < 0 ||
        config.equilibrium_bps-config.entry_offset_bps <= -bps || config.participation > 1 ||
        config.funding_cancel_ms < 0 || config.funding_cancel_ms >= default_interval_ms ||
        config.max_spot_age_ms < 0 || config.max_perp_age_ms < 0 ||
        quantity(config.max_gross_perp_btc > 0 ? std::min(config.order_btc,config.max_gross_perp_btc) : config.order_btc) < config.quantity_step)
        throw std::runtime_error("Invalid basis equilibrium, cancellation window, age or quantity");
    if (config.center_halflife_ms < 0 || config.max_hold_ms < 0 ||
        !std::isfinite(config.funding_exit_min_bps) || config.funding_exit_min_bps < 0)
        throw std::runtime_error("Center half-life, maximum hold and funding exit minimum must be nonnegative");
}
double Engine::quantity(double size) const {
    return std::floor(std::max(0.0,size)/config_.quantity_step+1e-9)*config_.quantity_step;
}
bool Engine::ready() const {
    return spot_time_ >= 0 && perp_time_ >= 0 && spot_bid_ > 0 && spot_ask_ >= spot_bid_ && perp_bid_ > 0 && perp_ask_ >= perp_bid_ &&
        now_-spot_time_ <= config_.max_spot_age_ms && now_-perp_time_ <= config_.max_perp_age_ms;
}
double Engine::basis() const {
    return ((perp_bid_+perp_ask_)/(spot_bid_+spot_ask_)-1)*bps;
}
int Engine::entry_side(double observed_basis) const {
    // Boundaries are inclusive. A zero offset preserves the old policy of
    // never quoting exactly at equilibrium, where neither side has an edge.
    constexpr double tolerance = 1e-9;
    const double center = center_bps();
    if (observed_basis < center-tolerance && observed_basis <= center-config_.entry_offset_bps+tolerance) return 1;
    if (observed_basis > center+tolerance && observed_basis >= center+config_.entry_offset_bps-tolerance) return -1;
    return 0;
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
std::array<SideTotals,2> Engine::side_totals() const {
    std::array<SideTotals,2> result;
    const double spread = final_perp_mid()-final_spot_mid();
    for (std::size_t i=0;i<result.size();++i) {
        const auto& a = sides_[i]; auto& t = result[i];
        t.entries = a.entries; t.closed_pairs = a.closed_pairs;
        t.open_btc = a.entries == a.closed_pairs ? 0 : (i == 0 ? open_long_btc_ : open_short_btc_);
        t.unrealized_pnl = a.open_entry_cash+(i == 0 ? 1 : -1)*t.open_btc*spread-a.open_entry_fees;
        t.realized_price_pnl = a.realized_price_pnl; t.funding_pnl = a.funding_pnl; t.fees = a.fees;
    }
    return result;
}
double Engine::used_capacity() const {
    return (config_.exit_on_equilibrium || config_.exit_before_funding) ? open_gross_btc_ : gross_btc_;
}
double Engine::available_capacity() const {
    return config_.max_gross_perp_btc > 0 ? config_.max_gross_perp_btc-used_capacity() : config_.order_btc;
}
void Engine::record_fill(Fill fill) {
    auto& stats = fill_stats_; ++stats.count;
    if (fill.leg == "SPOT") stats.spot_taker_notional += fill.notional;
    else {
        (fill.liquidity == "MAKER" ? stats.perp_maker_notional : stats.perp_taker_notional) += fill.notional;
        if (fill.phase == "ENTRY") { ++stats.open; stats.open_gross += fill.btc; }
        else { --stats.open; stats.open_gross -= fill.btc; }
        stats.max_open = std::max(stats.max_open,stats.open);
        stats.max_open_gross = std::max(stats.max_open_gross,stats.open_gross);
    }
    if (config_.retain_fills) fills_.push_back(std::move(fill));
}
void Engine::close_converged() {
    if (!config_.exit_on_equilibrium || open_pairs_.empty() || !ready()) return;
    const double observed_basis = basis(), center = center_bps();
    // Every live pair has the same direction: crossing the shared equilibrium
    // closes that side before opposite entries become eligible. Avoid scanning
    // all live lots on every tick when no crossing occurred.
    const int direction = pairs_[open_pairs_.front()].direction;
    if (direction > 0 ? observed_basis < center-1e-9 : observed_basis > center+1e-9) return;
    auto keep = open_pairs_.begin();
    for (auto index : open_pairs_) {
        auto& p = pairs_[index];
        const bool converged = p.direction > 0 ? observed_basis >= center-1e-9 : observed_basis <= center+1e-9;
        if (!converged) { *keep++ = index; continue; }
        close_pair(p,"EQUILIBRIUM");
    }
    open_pairs_.erase(keep,open_pairs_.end());
    if (direction > 0) open_long_btc_ = 0; else open_short_btc_ = 0;
    if (std::abs(open_gross_btc_) < 1e-10) open_gross_btc_ = 0;
    if (std::abs(perp_btc_) < 1e-10) perp_btc_ = 0;
    if (std::abs(spot_btc_) < 1e-10) spot_btc_ = 0;
}
void Engine::close_expired() {
    if (config_.max_hold_ms <= 0 || open_pairs_.empty() || !ready()) return;
    auto keep = open_pairs_.begin();
    for (auto index : open_pairs_) {
        auto& p = pairs_[index];
        if (now_-p.time < config_.max_hold_ms) { *keep++ = index; continue; }
        close_pair(p,"MAX_HOLD");
    }
    open_pairs_.erase(keep,open_pairs_.end());
    if (std::abs(open_gross_btc_) < 1e-9) open_gross_btc_ = 0;
}
void Engine::update_center() {
    // Time-weighted EMA of the mid basis, using the books in force since the last update.
    if (config_.center_halflife_ms <= 0 || spot_time_ < 0 || perp_time_ < 0 || spot_bid_ <= 0 || perp_bid_ <= 0) return;
    const double observed_basis = basis();
    if (center_time_ < 0) { center_ = observed_basis; center_start_ = center_time_ = now_; return; }
    const double weight = 1-std::exp(-std::log(2.0)*static_cast<double>(now_-center_time_)/config_.center_halflife_ms);
    center_ += weight*(observed_basis-center_);
    center_time_ = now_;
}
void Engine::close_pair(Pair& p, const std::string& reason) {
    // Both reduce-only legs cross the current executable side, even in the
    // pre-funding entry blackout. A midpoint crossing is not a free fill.
    const double perp_price = p.direction > 0 ? perp_bid_ : perp_ask_;
    const double spot_price = p.direction > 0 ? spot_ask_ : spot_bid_;
    const double perp_fee = p.btc*perp_price*config_.perp_taker_bps/bps;
    const double spot_fee = p.btc*spot_price*config_.spot_taker_bps/bps;
    record_fill({now_,p.id,"PERP","TAKER",-p.direction,perp_price,p.btc,p.btc*perp_price,perp_fee,"EXIT"});
    record_fill({now_,p.id,"SPOT","TAKER",p.direction,spot_price,p.btc,p.btc*spot_price,spot_fee,"EXIT"});
    cash_ += p.direction*p.btc*(perp_price-spot_price);
    perp_btc_ -= p.direction*p.btc; spot_btc_ += p.direction*p.btc;
    open_gross_btc_ -= p.btc;
    (p.direction > 0 ? open_long_btc_ : open_short_btc_) -= p.btc;
    fees_ += perp_fee+spot_fee;
    p.exit_reason = reason;
    p.closed = true; p.exit_time = now_; p.exit_perp_price = perp_price;
    p.exit_spot_price = spot_price; p.exit_fees = perp_fee+spot_fee;
    p.realized_pnl = p.direction*p.btc*((perp_price-p.perp_price)-(spot_price-p.spot_price))-p.fees-p.exit_fees+p.funding_pnl;
    auto& attribution = sides_[p.direction > 0 ? 0 : 1];
    ++attribution.closed_pairs;
    attribution.open_entry_cash -= p.direction*p.btc*(p.spot_price-p.perp_price);
    attribution.open_entry_fees -= p.fees;
    attribution.realized_price_pnl += p.realized_pnl-p.funding_pnl;
    attribution.fees += p.exit_fees;
    if (attribution.entries == attribution.closed_pairs) {
        attribution.open_entry_cash = 0; attribution.open_entry_fees = 0;
    }
    // Funding cash was realized when paid, even while this pair was open.
    realized_pnl_ += p.realized_pnl-p.funding_pnl;
    quote_side_ = 0; remaining_ = 0;
}
void Engine::close_before_funding() {
    // Causal estimate from completed official premium-index minutes only.
    if (!config_.exit_before_funding || open_pairs_.empty() || !ready() || next_funding_ < 0 ||
        now_ < next_funding_-config_.funding_cancel_ms || now_ >= next_funding_ || !premium_ready()) return;
    const double rate = estimated_funding_bps();
    // Even the flat +1 bp rate is a payment unless funding_exit_min_bps exempts it.
    if (rate == 0 || std::abs(rate) < config_.funding_exit_min_bps) return;
    const int payer = rate > 0 ? 1 : -1;
    if (config_.funding_exit_payers_only && (payer > 0 ? open_long_btc_ : open_short_btc_) < 1e-10) return;
    auto keep = open_pairs_.begin();
    for (auto index : open_pairs_) {
        auto& p = pairs_[index];
        if (config_.funding_exit_payers_only && p.direction != payer) { *keep++ = index; continue; }
        close_pair(p,"PRE_FUNDING_PAYER");
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
    const char* reason = "MISSING_OR_STALE_MARKET";
    if (ready()) {
        observed_basis = basis();
        if (funding_guard()) reason = "PRE_FUNDING_CANCEL";
        else if (config_.center_halflife_ms > 0 && (center_start_ < 0 || now_-center_start_ < config_.center_halflife_ms))
            reason = "CENTER_WARMUP";
        else if (quantity(available_capacity()) < config_.quantity_step)
            reason = (config_.exit_on_equilibrium || config_.exit_before_funding) ? "OPEN_POSITION_CAP" : "GROSS_POSITION_CAP";
        else {
            side = entry_side(observed_basis);
            if (side) price = side > 0 ? perp_bid_ : perp_ask_;
            else reason = "WITHIN_ENTRY_BAND";
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
        if (!config_.retain_decisions) decisions_.clear();
        decisions_.push_back({now_,reason,side,observed_basis,price,remaining_});
    }
}
void Engine::trade(const Event& e) {
    if (!ready() || funding_guard() || !quote_side_ || e.time <= quote_since_) return;
    const double observed_basis = basis();
    if (entry_side(observed_basis) != quote_side_) return;
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
    auto& attribution = sides_[direction > 0 ? 0 : 1];
    ++attribution.entries;
    attribution.open_entry_cash += direction*q*(spot_price-perp_price);
    attribution.open_entry_fees += perp_fee+spot_fee;
    attribution.fees += perp_fee+spot_fee;
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
    record_fill({now_,id,"PERP","MAKER",direction,perp_price,q,q*perp_price,perp_fee});
    record_fill({now_,id,"SPOT","TAKER",-direction,spot_price,q,q*spot_price,spot_fee});
    pairs_.push_back({id,now_,direction,q,perp_price,spot_price,(perp_price/spot_price-1)*bps,perp_fee+spot_fee});
    pairs_.back().signal_basis_bps = observed_basis;
    pairs_.back().center_bps = center_bps();
    open_pairs_.push_back(pairs_.size()-1);
}
void Engine::on_event(const Event& e, bool) {
    if (e.time < 0 || e.time < now_) throw std::runtime_error("Basis events must be chronological");
    for (double v : {e.price,e.quantity,e.bid,e.ask,e.funding_rate,e.interval_hours})
        if (!std::isfinite(v)) throw std::runtime_error("Basis event values must be finite");
    now_ = e.time;
    update_center();
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
            sides_[0].funding_pnl -= open_long_btc_*e.price*e.funding_rate;
            sides_[1].funding_pnl += open_short_btc_*e.price*e.funding_rate;
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
    close_expired();
    close_before_funding();
    decide(); // Zero latency: references, exits and entry cancellations apply immediately.
    const auto t = totals();
    peak_ = std::max(peak_,t.net()); drawdown_ = std::max(drawdown_,peak_-t.net());
    if (ready() && (last_equity_ < 0 || now_/60000 > last_equity_/60000)) {
        equity_.push_back({now_,t,side_totals()}); last_equity_ = now_;
    }
}
void Engine::finish() {
    if (config_.close_at_end && perp_bid_ > 0 && spot_bid_ > 0) {
        for (auto index : open_pairs_) close_pair(pairs_[index],"END_OF_DATA");
        open_pairs_.clear(); open_long_btc_ = 0; open_short_btc_ = 0; open_gross_btc_ = 0;
        if (std::abs(perp_btc_) < 1e-9) perp_btc_ = 0;
        if (std::abs(spot_btc_) < 1e-9) spot_btc_ = 0;
    }
    quote_side_ = 0; remaining_ = 0;
    mark_equity(); // No exit or artificial closing fill.
}
void Engine::mark_equity() {
    if (now_ >= 0) equity_.push_back({now_,totals(),side_totals()});
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
    pairs << "pair_id,time_ms,direction,perp_btc,spot_btc,perp_price,spot_price,perp_notional,spot_notional,notional_difference,entry_basis_bps,entry_fees,status,exit_time_ms,exit_perp_price,exit_spot_price,exit_fees,realized_pnl,funding_pnl,exit_reason,signal_basis_bps,center_bps\n";
    for (const auto& p : engine.pairs()) pairs << p.id << ',' << p.time << ',' << (p.direction > 0 ? "LONG_PERP_SHORT_SPOT" : "SHORT_PERP_LONG_SPOT") << ','
        << p.btc << ',' << p.btc << ',' << p.perp_price << ',' << p.spot_price << ',' << p.btc*p.perp_price << ',' << p.btc*p.spot_price << ','
        << p.btc*(p.perp_price-p.spot_price) << ',' << p.entry_basis_bps << ',' << p.fees << ','
        << (p.closed ? "CLOSED" : "OPEN") << ',' << p.exit_time << ',' << p.exit_perp_price << ',' << p.exit_spot_price
        << ',' << p.exit_fees << ',' << p.realized_pnl << ',' << p.funding_pnl << ',' << p.exit_reason << ',' << p.signal_basis_bps << ',' << p.center_bps << '\n';
    quotes << "time_ms,status,side,perp_mid_basis_bps,price,remaining_btc\n";
    for (const auto& q : engine.decisions()) quotes << q.time << ',' << q.reason << ',' << q.side << ',' << q.perp_mid_basis_bps << ',' << q.price << ',' << q.remaining << '\n';
    }
    const std::array<const char*,2> names = {"long_perp_short_spot","short_perp_long_spot"};
    equity << "time_ms,price_pnl,fees,funding_pnl,net_pnl,perp_btc,spot_btc,delta_btc,gross_perp_btc,open_gross_perp_btc,realized_pnl,unrealized_pnl";
    for (const auto* name : names) for (const auto* field : {"unrealized_pnl","realized_price_pnl","funding_pnl","fees","net_pnl","open_btc"}) equity << ',' << name << '_' << field;
    equity << '\n';
    for (const auto& e : engine.equity()) {
        equity << e.time << ',' << e.totals.price_pnl << ',' << e.totals.fees << ',' << e.totals.funding_pnl << ','
        << e.totals.net() << ',' << e.totals.perp_btc << ',' << e.totals.spot_btc << ',' << e.totals.perp_btc+e.totals.spot_btc << ',' << e.totals.gross_perp_btc << ',' << e.totals.open_gross_perp_btc
        << ',' << e.totals.realized_pnl << ',' << e.totals.net()-e.totals.realized_pnl;
        for (const auto& side : e.sides) equity << ',' << side.unrealized_pnl << ',' << side.realized_price_pnl << ',' << side.funding_pnl << ',' << side.fees << ',' << side.net() << ',' << side.open_btc;
        equity << '\n';
    }
    const auto& c = engine.config(); const auto t = engine.totals();
    std::size_t closed = 0, wins = 0, losses = 0, funding_closed = 0, convergence_closed = 0, entry_violations = 0;
    std::size_t hold_closed = 0, end_closed = 0;
    double closed_bps = 0, closed_hold_ms = 0;
    for (const auto& p : engine.pairs()) {
        const auto& config = engine.config();
        entry_violations += p.direction > 0 ? p.signal_basis_bps > p.center_bps-config.entry_offset_bps+1e-9
                                           : p.signal_basis_bps < p.center_bps+config.entry_offset_bps-1e-9;
        if (p.closed) {
            ++closed; wins += p.realized_pnl > 1e-10; losses += p.realized_pnl < -1e-10;
            funding_closed += p.exit_reason == "PRE_FUNDING_PAYER"; convergence_closed += p.exit_reason == "EQUILIBRIUM";
            hold_closed += p.exit_reason == "MAX_HOLD"; end_closed += p.exit_reason == "END_OF_DATA";
            closed_bps += p.realized_pnl/(p.btc*p.spot_price)*bps; closed_hold_ms += p.exit_time-p.time;
        }
    }
    const auto& stats = engine.fill_stats();
    const double perp_maker_notional = stats.perp_maker_notional,perp_taker_notional = stats.perp_taker_notional,
                 spot_taker_notional = stats.spot_taker_notional;
    std::ofstream summary(directory+"/summary.json");
    summary << std::fixed << std::setprecision(10) << "{\n"
        << "  \"strategy\": \"passive perp basis entry, immediate equal-BTC spot hedge\",\n"
        << "  \"equilibrium_bps\": " << c.equilibrium_bps << ",\n  \"latency_ms\": 0,\n  \"exits_enabled\": " << ((c.exit_on_equilibrium || c.exit_before_funding) ? "true" : "false") << ",\n"
        << "  \"entry_offset_bps\": " << c.entry_offset_bps << ",\n  \"entry_bid_basis_bps\": " << c.equilibrium_bps-c.entry_offset_bps
        << ",\n  \"entry_ask_basis_bps\": " << c.equilibrium_bps+c.entry_offset_bps << ",\n"
        << "  \"entry_signal\": \"10000 * (perp_mid / spot_mid - 1)\",\n  \"entry_band_violations\": " << entry_violations << ",\n"
        << "  \"exit_execution\": \"" << (c.exit_on_equilibrium ? "Equilibrium crossings and any enabled funding exits" : c.exit_before_funding ? "Any estimated funding payer during final pre-settlement window, including flat funding region" : "No exits") << "\",\n"
        << "  \"capacity_rule\": \"" << (c.max_gross_perp_btc == 0 ? "Unlimited concurrent entry capacity" : (c.exit_on_equilibrium || c.exit_before_funding) ? "Open gross perp BTC; exits release capacity" : "Cumulative gross perp BTC; capacity not recycled") << "\",\n"
        << "  \"funding_assumption\": \"" << (c.funding_cashflows ? "Recorded settlement cash flows on held positions" : "Zero funding cash flows") << "\",\n  \"funding_cancel_seconds\": " << c.funding_cancel_ms/1000.0 << ",\n"
        << "  \"perp_maker_bps\": " << c.perp_maker_bps << ",\n  \"perp_taker_bps\": " << c.perp_taker_bps << ",\n  \"spot_taker_bps\": " << c.spot_taker_bps << ",\n"
        << "  \"cycle_fee_bps\": " << c.perp_maker_bps+c.perp_taker_bps+2*c.spot_taker_bps << ",\n"
        << "  \"exit_on_equilibrium\": " << (c.exit_on_equilibrium ? "true" : "false") << ",\n"
        << "  \"exit_before_funding\": " << (c.exit_before_funding ? "true" : "false") << ",\n"
        << "  \"funding_exit_payers_only\": " << (c.funding_exit_payers_only ? "true" : "false") << ",\n"
        << "  \"funding_exit_flat_region_exempt\": false,\n"
        << "  \"funding_cashflows\": " << (c.funding_cashflows ? "true" : "false") << ",\n"
        << "  \"funding_interest_bps\": " << c.funding_interest_bps << ",\n  \"funding_clamp_bps\": " << c.funding_clamp_bps << ",\n"
        << "  \"funding_exit_pairs\": " << funding_closed << ",\n  \"convergence_exit_pairs\": " << convergence_closed << ",\n"
        << "  \"max_hold_exit_pairs\": " << hold_closed << ",\n  \"end_of_data_exit_pairs\": " << end_closed << ",\n"
        << "  \"closed_pair_mean_net_bps\": " << (closed ? closed_bps/closed : 0) << ",\n"
        << "  \"closed_pair_mean_hold_seconds\": " << (closed ? closed_hold_ms/closed/1000 : 0) << ",\n"
        << "  \"center_halflife_seconds\": " << c.center_halflife_ms/1000.0 << ",\n  \"max_hold_seconds\": " << c.max_hold_ms/1000.0 << ",\n"
        << "  \"close_at_end\": " << (c.close_at_end ? "true" : "false") << ",\n  \"funding_exit_min_bps\": " << c.funding_exit_min_bps << ",\n"
        << "  \"funding_signal\": \"Causal weighted average of completed 1m premium-index CLOSE proxies; not exact exchange forecast\",\n"
        << "  \"order_btc\": " << c.order_btc << ",\n  \"max_gross_perp_btc\": " << c.max_gross_perp_btc << ",\n"
        << "  \"participation\": " << c.participation << ",\n  \"fill_at_touch\": " << (c.fill_at_touch ? "true" : "false") << ",\n"
        << "  \"proxy_spot_half_spread_bps\": " << c.proxy_spot_half_spread_bps << ",\n  \"spot_proxy_events\": " << engine.spot_proxy_events() << ",\n"
        << "  \"spot_mid_events\": " << engine.spot_mid_events() << ",\n  \"paired_partial_fills\": " << engine.pairs().size() << ",\n"
        << "  \"leg_fills\": " << stats.count << ",\n  \"price_pnl\": " << t.price_pnl << ",\n  \"fees\": " << t.fees << ",\n"
        << "  \"perp_maker_notional\": " << perp_maker_notional << ",\n  \"perp_taker_notional\": " << perp_taker_notional
        << ",\n  \"spot_taker_notional\": " << spot_taker_notional << ",\n"
        << "  \"closed_pairs\": " << closed << ",\n  \"open_pairs\": " << engine.pairs().size()-closed << ",\n"
        << "  \"winning_closed_pairs\": " << wins << ",\n  \"losing_closed_pairs\": " << losses << ",\n"
        << "  \"max_concurrent_pairs\": " << stats.max_open << ",\n  \"max_open_gross_perp_btc\": " << stats.max_open_gross << ",\n"
        << "  \"realized_pnl\": " << t.realized_pnl << ",\n  \"unrealized_pnl\": " << t.net()-t.realized_pnl << ",\n"
        << "  \"funding_pnl\": " << t.funding_pnl << ",\n  \"net_pnl\": " << t.net() << ",\n  \"perp_btc\": " << t.perp_btc << ",\n  \"spot_btc\": " << t.spot_btc << ",\n"
        << "  \"final_perp_mid\": " << engine.final_perp_mid() << ",\n  \"final_spot_mid\": " << engine.final_spot_mid() << ",\n"
        << "  \"delta_btc\": " << t.perp_btc+t.spot_btc << ",\n  \"gross_perp_btc\": " << t.gross_perp_btc << ",\n"
        << "  \"open_gross_perp_btc\": " << t.open_gross_perp_btc << ",\n  \"max_drawdown\": " << engine.max_drawdown() << ",\n"
        << "  \"limitations\": \"Open positions marked at mids; closed pairs use both executable exit sides and all four leg fees. Spot price-only input uses last-trade proxy with assumed spread. Borrow availability, interest, impact, depth, margin and liquidation are not modeled. No queue reconstruction. Midpoint convergence does not guarantee a positive executable round trip.\",\n"
        << "  \"basis_sides\": {\n";
    const auto sides = engine.side_totals();
    for (std::size_t i=0;i<sides.size();++i) {
        const auto& side = sides[i];
        summary << "    \"" << names[i] << "\": {\"entries\": " << side.entries << ", \"closed_pairs\": " << side.closed_pairs
            << ", \"open_pairs\": " << side.entries-side.closed_pairs << ", \"open_btc\": " << side.open_btc
            << ", \"unrealized_pnl\": " << side.unrealized_pnl << ", \"realized_price_pnl\": " << side.realized_price_pnl
            << ", \"funding_pnl\": " << side.funding_pnl << ", \"fees\": " << side.fees << ", \"net_pnl\": " << side.net()
            << '}' << (i+1<sides.size() ? "," : "") << '\n';
    }
    summary << "  }\n}\n";
    if ((details && (!fills || !pairs || !quotes)) || !equity || !summary) throw std::runtime_error("Failed writing paired basis results");
}
} // namespace btc::basis_maker
