#include "basis-backtest.h"
#include <cmath>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
double number(const std::string& text) {
    std::size_t count = 0;
    const double value = std::stod(text,&count);
    if (count != text.size() || !std::isfinite(value)) throw std::runtime_error("Invalid number");
    return value;
}
}
int main(int argc,char** argv) {
    try {
        btc::backtest::Settings settings;
        // Defaults assume running from Primary-Market-Model (including make backtest).
        std::string spot = "../data/coinbase/BTC-USD_spot_1m.csv";
        std::string perp = "../data/coinbase/BTC-PERP-INTX_1m.csv";
        std::string funding = "../data/coinbase/BTC-PERP-INTX_funding_1h.csv";
        std::string results = "results/basis-both-5m";
        bool hold_given = false;
        for (int i = 1; i < argc; ++i) {
            const std::string option = argv[i];
            if (option == "--help") {
                std::cout << "Zero-cost basis backtest: buy below -5 bps; sell above +5 bps.\n"
                    << "--spot-csv FILE --perp-csv FILE --funding-csv FILE --output DIRECTORY\n"
                    << "--hold-minutes 5 --threshold-bps 5 --qty-btc 0.01 --min-edge-bps 0\n"
                    << "--profit-exit 1 (0 waits for convergence or timeout)\n"
                    << "--exit-basis-bps X (optional forecast; default targets -5 long / +5 short)\n"
                    << "--strategy both-basis (default), long-basis, short-basis, or theo-carry\n"
                    << "Exit on first positive PnL, convergence to the respective threshold, or five-minute cap.\n"
                    << "One position at a time; minute-open prices; recorded funding.\n"
                    << "Past 60 completed candle closes proxy the funding premium TWAP.\n"
                    << "BTC-USD and USDC quotes are assumed at parity. No fees or execution costs.\n";
                return 0;
            }
            if (++i >= argc) throw std::runtime_error("Missing value for " + option);
            const std::string value = argv[i];
            if (option == "--spot-csv") spot = value;
            else if (option == "--perp-csv") perp = value;
            else if (option == "--funding-csv") funding = value;
            else if (option == "--output") results = value;
            else if (option == "--qty-btc") settings.quantity_btc = number(value);
            else if (option == "--min-edge-bps") settings.minimum_edge_bps = number(value);
            else if (option == "--discount-bps" || option == "--threshold-bps") settings.discount_threshold_bps = number(value);
            else if (option == "--profit-exit") {
                if (value != "0" && value != "1") throw std::runtime_error("--profit-exit must be 0 or 1");
                settings.exit_on_profit = value == "1";
            }
            else if (option == "--strategy") {
                if (value == "both-basis") settings.strategy = btc::backtest::Strategy::BothBasis;
                else if (value == "long-basis") settings.strategy = btc::backtest::Strategy::LongBasis;
                else if (value == "short-basis") settings.strategy = btc::backtest::Strategy::ShortBasis;
                else if (value == "theo-carry") settings.strategy = btc::backtest::Strategy::TheoCarry;
                else throw std::runtime_error("Unknown strategy");
            }
            else if (option == "--exit-basis-bps") settings.forecast_exit_basis_bps = number(value);
            else if (option == "--hold-hours" || option == "--hold-minutes") {
                const double minutes = number(value)*(option == "--hold-hours" ? 60 : 1);
                if (minutes <= 0 || minutes > 525600 || std::abs(minutes-std::round(minutes)) > 1e-8)
                    throw std::runtime_error("Holding period must be a positive whole number of minutes");
                settings.holding_minutes = static_cast<int>(std::round(minutes)); hold_given = true;
            } else throw std::runtime_error("Unknown option: " + option);
        }
        if (settings.strategy == btc::backtest::Strategy::TheoCarry && !hold_given) settings.holding_minutes = 1440;
        const auto data = btc::backtest::load(spot,perp,funding);
        const auto run = btc::backtest::run(data,settings);
        btc::backtest::write_results(data,settings,run,results);
        std::cout << std::fixed << std::setprecision(6)
            << "HISTORICAL ZERO-COST BASIS BACKTEST\n"
            << "UTC period: " << btc::backtest::utc(data.prices.begin()->first) << " through "
            << btc::backtest::utc(data.prices.rbegin()->first) << '\n'
            << "Strategy: " << btc::backtest::strategy_name(settings.strategy) << '\n'
            << "Maximum hold: " << settings.holding_minutes << " minutes; size: " << settings.quantity_btc << " BTC per leg\n"
            << "Trades: " << run.trades.size() << " (long basis " << run.long_trades << ", short basis " << run.short_trades << ")\n"
            << "BASIS PRICE PnL: " << run.price_pnl << '\n'
            << "REALIZED FUNDING PnL: " << run.funding_pnl << '\n'
            << "TOTAL PnL: " << run.net_pnl << '\n'
            << "Mean trade PnL: " << run.mean_trade_bps << " bps of entry spot notional\n"
            << "Win rate: " << (run.trades.empty() ? 0 : 100.0*run.wins/run.trades.size()) << "%\n"
            << "Maximum mark-to-market drawdown: " << run.max_drawdown << '\n'
            << "Missing matched minutes: " << run.missing_minutes << "; skipped feature checks: " << run.skipped_feature_hours
            << "; delayed exits: " << run.delayed_exits << '\n'
            << "Results: " << results << "/{summary.json,trades.csv,equity.csv}\n"
            << "Prices are idealized minute opens; TWAP uses trailing minute closes; USD/USDC assumed at parity.\n"
            << "The last open pair is liquidated at data end. This does not simulate resting-order fills or liquidations.\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "Error: " << e.what() << '\n'; return 1; }
}
