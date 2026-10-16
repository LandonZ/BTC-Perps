#pragma once
#include <string>

namespace btc::maker {
enum class QuoteRule { SpotFee, CostBudget };
struct Inputs {
    double spot = 0, perp_bid = 0, perp_ask = 0;
    double funding_bps = 0; // Signed per-settlement rate known at decision time.
    bool funding_ready = false;
    double minutes_to_funding = 480;
    double center_bps = 0, inventory_contracts = 0;
};
struct Config {
    QuoteRule rule = QuoteRule::SpotFee;
    double maker_fee_bps = 2, fee_usd_per_contract = 0;
    double contract_btc = 0.01, tick = 0.1;
    double funding_interval_minutes = 480;
    double max_abs_funding_bps = 1; // Example policy, not a fitted threshold.
    double min_basis_bps = -5, max_basis_bps = 5; // Perp midpoint vs spot; also bounds posted prices.
    double horizon_minutes = 5;
    double risk_buffer_bps = 0, profit_bps = 0;
    double max_spread_bps = 20; // Policy ceiling on full quote width.
    double skew_bps_per_contract = 0;
};
struct Quotes {
    bool enabled = false;
    bool bid_enabled = false, ask_enabled = false;
    std::string reason = "FUNDING_WARMUP";
    double reference = 0, bid = 0, ask = 0;
    double observed_basis_bps = 0, lower_price_bound = 0, upper_price_bound = 0;
    double funding_reserve_bps = 0;
    double minimum_spread_bps = 0, quoted_spread_bps = 0;
    double cycle_fees_bps = 0, expected_net_bps = 0;
};
void validate(const Config& config);
// A static bid/ask cycle budget, conditional on both sides filling. It is not
// a forecast of realized PnL when spot, quotes or inventory change between fills.
Quotes price(const Inputs& inputs, const Config& config);
} // namespace btc::maker
