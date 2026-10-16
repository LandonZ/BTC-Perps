#include "maker-model.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace btc::maker {
namespace {
constexpr double bps = 10000;
double down(double price, double tick) { return std::floor(price/tick + 1e-9)*tick; }
double up(double price, double tick) { return std::ceil(price/tick - 1e-9)*tick; }
}
void validate(const Config& c) {
    for (double v : {c.maker_fee_bps,c.fee_usd_per_contract,c.contract_btc,c.tick,
                    c.funding_interval_minutes,c.max_abs_funding_bps,c.horizon_minutes,c.risk_buffer_bps,
                    c.profit_bps,c.max_spread_bps,c.skew_bps_per_contract})
        if (!std::isfinite(v) || v < 0) throw std::runtime_error("Maker settings must be finite and nonnegative");
    if (c.contract_btc <= 0 || c.tick <= 0 || c.funding_interval_minutes <= 0 || c.max_spread_bps <= 0 ||
        c.maker_fee_bps >= bps || c.horizon_minutes > 525600)
        throw std::runtime_error("Invalid maker tick, contract size, fee, horizon or maximum spread");
    if (!std::isfinite(c.min_basis_bps) || !std::isfinite(c.max_basis_bps) ||
        c.min_basis_bps <= -bps || c.max_basis_bps <= c.min_basis_bps)
        throw std::runtime_error("Maker basis bounds must be finite, ordered, and above -10000 bps");
}
Quotes price(const Inputs& in, const Config& c) {
    validate(c);
    for (double v : {in.spot,in.perp_bid,in.perp_ask,in.funding_bps,in.minutes_to_funding,
                    in.center_bps,in.inventory_contracts})
        if (!std::isfinite(v)) throw std::runtime_error("Maker inputs must be finite");
    if (in.spot <= 0 || in.perp_bid <= 0 || in.perp_ask < in.perp_bid ||
        in.minutes_to_funding <= 0 || in.minutes_to_funding > c.funding_interval_minutes)
        throw std::runtime_error("Invalid maker market or time to funding");
    Quotes q;
    q.reference = c.rule == QuoteRule::SpotFee ? in.spot :
        in.spot*(1 + (in.center_bps-c.skew_bps_per_contract*in.inventory_contracts)/bps);
    if (q.reference <= 0) throw std::runtime_error("Maker reservation price must be positive");
    q.observed_basis_bps = ((in.perp_bid+in.perp_ask)/(2*in.spot)-1)*bps;
    q.lower_price_bound = up(in.spot*(1+c.min_basis_bps/bps),c.tick);
    q.upper_price_bound = down(in.spot*(1+c.max_basis_bps/bps),c.tick);
    if (q.observed_basis_bps < c.min_basis_bps-1e-9 || q.observed_basis_bps > c.max_basis_bps+1e-9) {
        q.reason = "BASIS_OUT_OF_RANGE"; return q;
    }
    if (c.rule == QuoteRule::SpotFee) {
        // User's rule: quote precisely spot +/- one maker fee, without carry,
        // discretionary profit/risk margins, inventory skew or rate prediction.
        q.reference = in.spot;
        const double fee = c.maker_fee_bps/bps;
        const double flat = c.fee_usd_per_contract/c.contract_btc;
        q.bid = down(in.spot*(1-fee)-flat,c.tick);
        q.ask = up(in.spot*(1+fee)+flat,c.tick);
        q.minimum_spread_bps = (2*in.spot*fee+2*flat)/in.spot*bps;
        q.quoted_spread_bps = (q.ask-q.bid)/in.spot*bps;
        q.cycle_fees_bps = (fee*(q.ask+q.bid)+2*flat)/in.spot*bps;
        q.expected_net_bps = q.quoted_spread_bps-q.cycle_fees_bps;
        if (q.bid < q.lower_price_bound-1e-9 || q.ask > q.upper_price_bound+1e-9) {
            q.reason = "FEE_QUOTES_OUT_OF_BAND"; return q;
        }
        if (q.bid <= 0 || q.ask <= q.bid) { q.reason = "INVALID_PRICE"; return q; }
        // Reject an immediately marketable side instead of moving its price.
        const double tolerance = c.tick*1e-7;
        q.bid_enabled = q.bid < in.perp_ask-tolerance;
        q.ask_enabled = q.ask > in.perp_bid+tolerance;
        q.enabled = q.bid_enabled || q.ask_enabled;
        q.reason = q.enabled ? "QUOTING" : "NO_PASSIVE_QUOTE_IN_BAND";
        return q;
    }
    if (!in.funding_ready) return q;
    if (std::abs(in.funding_bps) > c.max_abs_funding_bps) { q.reason = "HIGH_FUNDING"; return q; }
    // Reserve the adverse funding sign for either inventory direction. Include
    // full settlements crossed by the horizon, not a prorated funding charge.
    const double payments = c.horizon_minutes < in.minutes_to_funding ? 0 :
        1 + std::floor((c.horizon_minutes-in.minutes_to_funding)/c.funding_interval_minutes + 1e-12);
    q.funding_reserve_bps = std::abs(in.funding_bps)*payments*(in.perp_bid+in.perp_ask)/(2*in.spot);
    const double fee = c.maker_fee_bps/bps;
    const double flat = c.fee_usd_per_contract/c.contract_btc; // USD per BTC, per fill.
    const double reserve = in.spot*(q.funding_reserve_bps+c.risk_buffer_bps)/bps;
    const double half_cost = flat+reserve/2;
    // Exact percentage fees at each endpoint: ask*(1-fee)-bid*(1+fee)
    // must cover both flat fees, funding, risk and the requested profit.
    const double min_bid = down((q.reference-half_cost)/(1+fee),c.tick);
    const double min_ask = up((q.reference+half_cost)/(1-fee),c.tick);
    q.minimum_spread_bps = std::max(c.tick,min_ask-min_bid)/in.spot*bps;
    const double half_budget = half_cost+in.spot*c.profit_bps/(2*bps);
    q.bid = down(std::min((q.reference-half_budget)/(1+fee),in.perp_ask-c.tick),c.tick);
    q.ask = up(std::max((q.reference+half_budget)/(1-fee),in.perp_bid+c.tick),c.tick);
    if (q.ask <= q.bid) q.ask = q.bid+c.tick;
    // The economic spread must fit inside the chosen spot-basis band. Never
    // erase costs silently when clipping inventory-skewed or passive quotes.
    q.bid = std::max(q.bid,q.lower_price_bound);
    q.ask = std::min(q.ask,q.upper_price_bound);
    q.quoted_spread_bps = (q.ask-q.bid)/in.spot*bps;
    q.cycle_fees_bps = (fee*(q.ask+q.bid)+2*flat)/in.spot*bps;
    q.expected_net_bps = q.quoted_spread_bps-q.cycle_fees_bps-q.funding_reserve_bps-c.risk_buffer_bps;
    for (double v : {q.bid,q.ask,q.minimum_spread_bps,q.quoted_spread_bps,q.expected_net_bps})
        if (!std::isfinite(v)) throw std::runtime_error("Maker quote overflow");
    if (q.bid <= 0 || q.ask <= q.bid) q.reason = "INVALID_PRICE";
    else if (q.bid >= in.perp_ask-c.tick*1e-7 || q.ask <= in.perp_bid+c.tick*1e-7) q.reason = "NO_PASSIVE_QUOTE_IN_BAND";
    else if (q.quoted_spread_bps > c.max_spread_bps+1e-9) q.reason = "SPREAD_TOO_WIDE";
    else if (q.expected_net_bps+1e-9 < c.profit_bps) q.reason = "INSUFFICIENT_EDGE";
    else { q.enabled = q.bid_enabled = q.ask_enabled = true; q.reason = "QUOTING"; }
    return q;
}
} // namespace btc::maker
