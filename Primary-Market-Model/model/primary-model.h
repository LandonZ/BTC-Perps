#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace btc::directional {
using Timestamp = std::int64_t;
constexpr double bps = 10000;
constexpr std::size_t feature_count = 6;
using Features = std::array<double, feature_count>;

struct Snapshot {
    Timestamp ts = 0;
    double perp = 0, spot = 0;
    double perp_return_5m_bps = 0, spot_return_5m_bps = 0, perp_volatility_5m_bps = 0;
    double known_hourly_funding_bps = 0;
    double minutes_to_next_funding = 60;
};
struct Point { Timestamp ts = 0; double spot = 0, perp = 0; }; // Completed-candle availability time.
struct Funding { Timestamp ts = 0; double rate = 0, mark = 0; }; // Decimal hourly rate.
struct Dataset { std::vector<Point> points; std::vector<Funding> funding; };
struct Example {
    Timestamp ts = 0, end_ts = 0;
    Features x{};
    double target_return_bps = 0;
};
struct Metrics { std::size_t observations = 0; double mae_bps = 0, rmse_bps = 0, no_change_mae_bps = 0, no_change_rmse_bps = 0; };

struct ForecastModel {
    int horizon_minutes = 5;
    double ridge = 0.1;
    Features means{}, scales{};
    std::array<double, feature_count+1> coefficients{}; // Intercept, then standardized feature weights.
    double training_rmse_bps = 0;
    Metrics validation;
    double predict_return_bps(const Features& x) const;
    void save(const std::string& path) const;
    static ForecastModel load(const std::string& path);
};

struct Costs {
    // Explicit entry/exit rates. No default fees are assumed for the user's account.
    double entry_fee_bps = 0, exit_fee_bps = 0;
    double half_spread_bps = 1, slippage_bps_per_fill = 0.5;
    double financing_apr = 0.05, margin_fraction = 0.2;
    double min_net_edge_bps = 1;
};
struct ProfitEstimate {
    double predicted_perp = 0, funding_paid_per_btc = 0;
    double long_entry = 0, long_exit = 0, short_entry = 0, short_exit = 0;
    double long_fees = 0, short_fees = 0, financing = 0;
    double long_pnl = 0, short_pnl = 0, long_edge_bps = 0, short_edge_bps = 0;
    std::string decision;
};

Features features(const Snapshot& snapshot, int horizon_minutes);
double forecast_funding_per_btc(const Snapshot& snapshot, double predicted_perp,
                               int horizon_minutes, std::optional<double> cumulative_paid_bps = {});
ProfitEstimate profits(const Snapshot& snapshot, double predicted_perp, double funding_paid_per_btc,
                       double quantity_btc, int horizon_minutes, const Costs& costs);
Dataset load_dataset(const std::string& spot, const std::string& perp, const std::string& funding);
Snapshot snapshot_at(const Dataset& data, std::size_t index);
std::vector<Example> make_examples(const Dataset& data, int horizon_minutes);
ForecastModel fit(const std::vector<Example>& examples, int horizon_minutes, double ridge);
Metrics evaluate(const ForecastModel& model, const std::vector<Example>& examples);
} // namespace btc::directional
