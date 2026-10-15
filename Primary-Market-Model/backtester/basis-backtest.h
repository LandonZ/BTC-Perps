#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace btc::backtest {
using Time = std::int64_t;
struct Prices { double spot_open = 0, perp_open = 0, spot_close = 0, perp_close = 0; };
struct Funding { Time time = 0; double rate = 0, mark = 0; };
struct Dataset {
    std::map<Time, Prices> prices; // Candle START times; close is known only at time + 60.
    std::vector<Funding> funding;
    std::size_t spot_rows = 0, perp_rows = 0;
};
enum class Strategy { BothBasis, LongBasis, ShortBasis, TheoCarry };
const char* strategy_name(Strategy strategy);
struct Settings {
    Strategy strategy = Strategy::BothBasis;
    int holding_minutes = 5;
    double discount_threshold_bps = 5;
    bool exit_on_profit = true;
    double quantity_btc = 0.01, minimum_edge_bps = 0;
    std::optional<double> forecast_exit_basis_bps; // Empty = +/- threshold; carry mode persists entry basis.
};
struct Trade {
    Time entry = 0, exit = 0;
    int direction = 0; // +1 = long perp / short spot; -1 = short perp / long spot.
    double entry_spot = 0, entry_perp = 0, exit_spot = 0, exit_perp = 0;
    double entry_basis_bps = 0, exit_basis_bps = 0;
    double trailing_basis_bps = 0, previous_funding_bps = 0;
    double forecast_funding_bps = 0, forecast_edge_bps = 0;
    double price_pnl = 0, funding_pnl = 0, net_pnl = 0, net_bps = 0;
    int funding_payments = 0;
    bool forced_end = false;
    std::string exit_reason;
};
struct Equity { Time time = 0; double price_pnl = 0, funding_pnl = 0, net_pnl = 0; int direction = 0; };
struct Result {
    std::vector<Trade> trades;
    std::vector<Equity> equity;
    std::size_t missing_minutes = 0, skipped_feature_hours = 0, delayed_exits = 0;
    double price_pnl = 0, funding_pnl = 0, net_pnl = 0, max_drawdown = 0;
    double mean_trade_bps = 0;
    int wins = 0, long_trades = 0, short_trades = 0;
};
Dataset load(const std::string& spot_file, const std::string& perp_file, const std::string& funding_file);
Result run(const Dataset& data, const Settings& settings = {});
void write_results(const Dataset& data, const Settings& settings, const Result& result,
                   const std::string& directory);
std::string utc(Time time);
} // namespace btc::backtest
