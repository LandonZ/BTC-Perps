#pragma once
#include <vector>

namespace btc::theo {
constexpr double bps = 10000.0;

struct Inputs {
    double spot = 0; // Same quote currency as the linear perp (USDC).
    double expected_exit_basis_bps = 0; // Supplied trailing mean / simple forecast.
    double expected_funding_paid_bps = 0; // CUMULATIVE over the holding horizon.
    double inventory_btc = 0; // Positive = long basis: long perp / short spot.
    double perp_bid = 0, perp_ask = 0; // Optional current book for passive guards.
};

struct Config {
    double round_trip_fee_bps = 0; // Optional total fee budget for the complete trade.
    double round_trip_hedge_cost_bps = 0; // Optional execution-cost budget.
    double round_trip_profit_bps = 1; // Assumed target, not demonstrated profit.
    double long_carry_bps = 0, short_carry_bps = 0; // Optional side-specific budgets.
    double inventory_skew_bps_per_btc = 4;
    double quote_size_btc = 0.01;
    double max_inventory_btc = 0.10;
    double price_tick = 0.1;
    double size_increment_btc = 0.0001;
    double min_order_notional = 10;
};

struct Quotes {
    double theo = 0, theo_basis_bps = 0;
    double inventory_skew_bps = 0, reservation_price = 0;
    double half_width_bps = 0;
    double bid = 0, ask = 0, bid_size_btc = 0, ask_size_btc = 0;
    bool has_market = false;
    double observed_basis_bps = 0, market_mid_minus_theo_bps = 0;
};

// Simple legacy INTX forecast: persist the supplied hourly rate. New inventory
// starts after the current funding boundary; payments in (entry, exit] count.
double projected_hourly_funding_bps(double hourly_rate_bps, double horizon_minutes,
                                    double minutes_to_next_funding);
Quotes price(const Inputs& inputs, const Config& config = {});

// Scenario model using the funding rule in the user's reference image.
struct BasisInputs {
    double index = 0;
    double spot_bid = 0, spot_ask = 0;
    double perp_bid = 0, perp_ask = 0, last_trade = 0;
    double twap_basis_bps = 0; // One-hour TWAP of (mark/index - 1), BEFORE dividing by 24.
    double previous_funding_bps = 0;
    double exit_basis_bps = 0; // Assumed future perp minus hedge-spot midpoint.
    double horizon_minutes = 5, minutes_to_next_funding = 60;
};
struct BasisCosts {
    double quantity_btc = 0.01;
    double round_trip_fee_bps = 0; // Optional total fees, normalized to entry spot notional.
    double slippage_bps_per_fill = 0;
    double exit_spot_half_spread_bps = 0, exit_perp_half_spread_bps = 0;
    double financing_apr = 0, btc_borrow_apr = 0;
    double perp_margin_fraction = 0.20, min_net_edge_bps = 0;
};
struct BasisResult {
    double mark = 0, mark_index_basis_bps = 0, market_basis_bps = 0, next_funding_bps = 0;
    std::vector<double> funding_rates_bps;
    double long_funding_paid_per_btc = 0, theo = 0;
    double maximum_long_perp_ask = 0, minimum_short_perp_bid = 0;
    double fees = 0, financing = 0, btc_borrow = 0;
    double long_perp_price_pnl = 0, short_perp_price_pnl = 0;
    double long_perp_net_pnl = 0, short_perp_net_pnl = 0;
    // PnL in bps of entry spot notional, holding the spot reference fixed.
    double long_price_pnl_bps = 0, short_price_pnl_bps = 0;
    double long_funding_paid_bps = 0;
    double long_net_pnl_bps = 0, short_net_pnl_bps = 0;
    const char* candidate = "NEITHER";
};
double mark_price(double bid, double ask, double last);
double next_funding_bps(double twap_basis_bps, double previous_funding_bps);
BasisResult basis_theo(const BasisInputs& inputs, const BasisCosts& costs = {});
} // namespace btc::theo
