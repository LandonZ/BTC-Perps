#include "theo-model.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace btc::theo {
namespace {
void finite(double value, const char* label) {
    if (!std::isfinite(value)) throw std::runtime_error(label);
}
double down(double value, double increment) {
    return std::floor(value / increment + 1e-9) * increment;
}
double up(double value, double increment) {
    return std::ceil(value / increment - 1e-9) * increment;
}
} // namespace

double projected_hourly_funding_bps(double rate, double horizon, double next) {
    finite(rate, "Funding rate must be finite");
    finite(horizon, "Holding horizon must be finite");
    finite(next, "Time to funding must be finite");
    if (horizon < 0 || next <= 0 || next > 60)
        throw std::runtime_error("Horizon must be nonnegative; minutes to next hourly funding must be in (0,60]");
    if (horizon < next) return 0;
    const double payments = 1 + std::floor((horizon-next) / 60 + 1e-12);
    const double paid = payments * rate;
    finite(paid, "Projected funding overflowed");
    return paid;
}

Quotes price(const Inputs& in, const Config& c) {
    const double inputs[] = {in.spot, in.expected_exit_basis_bps, in.expected_funding_paid_bps,
                            in.inventory_btc, in.perp_bid, in.perp_ask};
    for (double v : inputs) finite(v, "Pricing inputs must be finite");
    const double nonnegative[] = {c.round_trip_fee_bps, c.round_trip_hedge_cost_bps,
        c.round_trip_profit_bps, c.long_carry_bps, c.short_carry_bps,
        c.inventory_skew_bps_per_btc, c.quote_size_btc, c.max_inventory_btc,
        c.price_tick, c.size_increment_btc, c.min_order_notional};
    for (double v : nonnegative) {
        finite(v, "Quote configuration must be finite");
        if (v < 0) throw std::runtime_error("Quote budgets and limits must be nonnegative");
    }
    if (in.spot <= 0 || in.expected_exit_basis_bps <= -bps || c.quote_size_btc <= 0 ||
        c.max_inventory_btc <= 0 || c.price_tick <= 0 || c.size_increment_btc <= 0)
        throw std::runtime_error("Prices, sizes and increments must be positive; exit basis must be above -10000 bps");
    const bool has_market = in.perp_bid != 0 || in.perp_ask != 0;
    if (has_market && (in.perp_bid <= 0 || in.perp_ask <= in.perp_bid))
        throw std::runtime_error("Provide both perp bid and ask, with 0 < bid < ask");

    Quotes q;
    // A horizon-specific hedged reservation value, before execution budgets.
    // Funding paid by a long lowers its value; funding received raises it.
    q.theo_basis_bps = in.expected_exit_basis_bps - in.expected_funding_paid_bps;
    q.theo = in.spot * (1 + q.theo_basis_bps / bps);
    q.inventory_skew_bps = in.inventory_btc * c.inventory_skew_bps_per_btc;
    q.reservation_price = q.theo - in.spot * q.inventory_skew_bps / bps;
    // The all-four-fill fee budget is allocated across a planned bid/ask cycle.
    // Add hedge costs and target profit ONCE to the full quoted spread.
    q.half_width_bps = (c.round_trip_fee_bps + c.round_trip_hedge_cost_bps +
                        c.round_trip_profit_bps) / 2;
    double bid = q.reservation_price - in.spot * (q.half_width_bps + c.long_carry_bps) / bps;
    double ask = q.reservation_price + in.spot * (q.half_width_bps + c.short_carry_bps) / bps;
    if (has_market) {
        bid = std::min(bid, in.perp_ask - c.price_tick);
        ask = std::max(ask, in.perp_bid + c.price_tick);
        q.observed_basis_bps = ((in.perp_bid + in.perp_ask) / (2 * in.spot) - 1) * bps;
        q.market_mid_minus_theo_bps = ((in.perp_bid + in.perp_ask) / 2 - q.theo) / in.spot * bps;
        q.has_market = true;
    }
    finite(q.theo, "Theo overflowed");
    finite(q.reservation_price, "Inventory reservation price overflowed");
    finite(bid, "Bid overflowed"); finite(ask, "Ask overflowed");
    q.bid = down(bid, c.price_tick);
    q.ask = up(ask, c.price_tick);
    if (q.theo <= 0 || q.bid <= 0 || q.ask <= q.bid)
        throw std::runtime_error("Configuration produced nonpositive or crossed quotes");
    q.bid_size_btc = down(std::min(c.quote_size_btc, std::max(0.0, c.max_inventory_btc-in.inventory_btc)),
                          c.size_increment_btc);
    q.ask_size_btc = down(std::min(c.quote_size_btc, std::max(0.0, c.max_inventory_btc+in.inventory_btc)),
                          c.size_increment_btc);
    finite(q.bid_size_btc, "Bid size overflowed");
    finite(q.ask_size_btc, "Ask size overflowed");
    if (q.bid_size_btc * q.bid < c.min_order_notional) q.bid_size_btc = 0;
    if (q.ask_size_btc * q.ask < c.min_order_notional) q.ask_size_btc = 0;
    return q;
}

double mark_price(double bid, double ask, double last) {
    for (double v : {bid,ask,last}) finite(v,"Mark inputs must be finite");
    if (bid <= 0 || ask < bid || last <= 0) throw std::runtime_error("Invalid perp book or last trade");
    return std::clamp(last,bid,ask); // Median(bid,ask,last); equality allows a zero-spread scenario.
}
double next_funding_bps(double twap_basis, double previous) {
    finite(twap_basis,"TWAP basis must be finite"); finite(previous,"Previous funding must be finite");
    return 0.75 * (twap_basis / 24.0) + 0.25 * previous;
}
BasisResult basis_theo(const BasisInputs& in,const BasisCosts& c) {
    for (double v : {in.index,in.spot_bid,in.spot_ask,in.perp_bid,in.perp_ask,in.last_trade,
                    in.twap_basis_bps,in.previous_funding_bps,in.exit_basis_bps,
                    in.horizon_minutes,in.minutes_to_next_funding}) finite(v,"Non-finite basis-model input");
    for (double v : {c.quantity_btc,c.round_trip_fee_bps,c.slippage_bps_per_fill,
                    c.exit_spot_half_spread_bps,c.exit_perp_half_spread_bps,c.financing_apr,
                    c.btc_borrow_apr,c.perp_margin_fraction,c.min_net_edge_bps}) {
        finite(v,"Non-finite basis-model cost");
        if (v < 0) throw std::runtime_error("Basis-model costs must be nonnegative");
    }
    if (in.index <= 0 || in.spot_bid <= 0 || in.spot_ask < in.spot_bid ||
        in.exit_basis_bps <= -bps || in.twap_basis_bps <= -bps || in.horizon_minutes < 0 ||
        in.horizon_minutes > 525600 || in.minutes_to_next_funding <= 0 || in.minutes_to_next_funding > 60 ||
        c.quantity_btc <= 0 || c.perp_margin_fraction <= 0 || c.perp_margin_fraction > 1 ||
        c.slippage_bps_per_fill >= bps || c.exit_spot_half_spread_bps+c.slippage_bps_per_fill >= bps ||
        c.exit_perp_half_spread_bps+c.slippage_bps_per_fill >= bps)
        throw std::runtime_error("Invalid basis-model prices, horizon, margin or execution costs");
    BasisResult r;
    r.mark = mark_price(in.perp_bid,in.perp_ask,in.last_trade);
    r.mark_index_basis_bps = (r.mark/in.index-1)*bps;
    const double spot = (in.spot_bid+in.spot_ask)/2;
    const double perp_mid = (in.perp_bid+in.perp_ask)/2;
    r.market_basis_bps = (perp_mid/spot-1)*bps;
    r.next_funding_bps = next_funding_bps(in.twap_basis_bps,in.previous_funding_bps);
    const int payments = in.horizon_minutes < in.minutes_to_next_funding ? 0 :
        1+static_cast<int>(std::floor((in.horizon_minutes-in.minutes_to_next_funding)/60+1e-12));
    double funding = in.previous_funding_bps;
    for (int i = 0; i < payments; ++i) {
        funding = next_funding_bps(in.twap_basis_bps,funding);
        r.funding_rates_bps.push_back(funding);
        // Scenario assumption: future hourly TWAP and settlement mark remain at supplied levels.
        r.long_funding_paid_per_btc += r.mark * funding/bps;
    }
    const double exit_basis_dollars = spot*in.exit_basis_bps/bps;
    const double exit_perp_mid = spot+exit_basis_dollars;
    r.theo = exit_perp_mid-r.long_funding_paid_per_btc;

    const double slip = c.slippage_bps_per_fill/bps;
    const double entry_perp_buy = in.perp_ask*(1+slip),entry_perp_sell = in.perp_bid*(1-slip);
    const double entry_spot_buy = in.spot_ask*(1+slip),entry_spot_sell = in.spot_bid*(1-slip);
    const double exit_perp_buy = exit_perp_mid*(1+(c.exit_perp_half_spread_bps+c.slippage_bps_per_fill)/bps);
    const double exit_perp_sell = exit_perp_mid*(1-(c.exit_perp_half_spread_bps+c.slippage_bps_per_fill)/bps);
    const double exit_spot_buy = spot*(1+(c.exit_spot_half_spread_bps+c.slippage_bps_per_fill)/bps);
    const double exit_spot_sell = spot*(1-(c.exit_spot_half_spread_bps+c.slippage_bps_per_fill)/bps);
    r.fees = c.quantity_btc*spot*c.round_trip_fee_bps/bps;
    const double fraction = in.horizon_minutes/(365.0*24*60);
    r.financing = c.quantity_btc*(spot+r.mark*c.perp_margin_fraction)*c.financing_apr*fraction;
    r.btc_borrow = c.quantity_btc*spot*c.btc_borrow_apr*fraction;
    r.long_perp_price_pnl = c.quantity_btc*((exit_perp_sell-entry_perp_buy)+(entry_spot_sell-exit_spot_buy));
    r.short_perp_price_pnl = c.quantity_btc*((entry_perp_sell-exit_perp_buy)+(exit_spot_sell-entry_spot_buy));
    const double fund_cash = c.quantity_btc*r.long_funding_paid_per_btc;
    r.long_perp_net_pnl = r.long_perp_price_pnl-fund_cash-r.fees-r.financing-r.btc_borrow;
    r.short_perp_net_pnl = r.short_perp_price_pnl+fund_cash-r.fees-r.financing;
    const double notional = c.quantity_btc*spot;
    r.long_price_pnl_bps = r.long_perp_price_pnl/notional*bps;
    r.short_price_pnl_bps = r.short_perp_price_pnl/notional*bps;
    r.long_funding_paid_bps = r.long_funding_paid_per_btc/spot*bps;
    r.long_net_pnl_bps = r.long_perp_net_pnl/notional*bps;
    r.short_net_pnl_bps = r.short_perp_net_pnl/notional*bps;
    // Entry-price bounds that cover ALL modeled costs and the requested edge buffer.
    const double fee_per_btc = r.fees/c.quantity_btc,carry = r.financing/c.quantity_btc;
    const double borrow = r.btc_borrow/c.quantity_btc,buffer = spot*c.min_net_edge_bps/bps;
    r.maximum_long_perp_ask = (entry_spot_sell+(exit_perp_sell-exit_spot_buy)-r.long_funding_paid_per_btc-
                               fee_per_btc-carry-borrow-buffer)/(1+slip);
    r.minimum_short_perp_bid = (entry_spot_buy+(exit_perp_buy-exit_spot_sell)-r.long_funding_paid_per_btc+
                               fee_per_btc+carry+buffer)/(1-slip);
    if (r.long_perp_net_pnl > c.quantity_btc*buffer && r.long_perp_net_pnl > r.short_perp_net_pnl)
        r.candidate = "LONG_PERP_SHORT_SPOT";
    else if (r.short_perp_net_pnl > c.quantity_btc*buffer && r.short_perp_net_pnl > r.long_perp_net_pnl)
        r.candidate = "SHORT_PERP_LONG_SPOT";
    for (double v : {r.mark,r.theo,r.long_funding_paid_per_btc,r.maximum_long_perp_ask,r.minimum_short_perp_bid,
                    r.long_perp_net_pnl,r.short_perp_net_pnl}) finite(v,"Basis-model calculation overflowed");
    return r;
}
} // namespace btc::theo
