#include "maker-backtest.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
double number(const std::string& text) {
    std::size_t used = 0;
    const double value = std::stod(text,&used);
    if (used != text.size() || !std::isfinite(value)) throw std::runtime_error("Invalid number: "+text);
    return value;
}
btc::maker_backtest::Millis milliseconds(const std::string& text) {
    const double value = number(text);
    if (value < 0 || value > 86400000 || value != std::floor(value))
        throw std::runtime_error("Time settings must be integer milliseconds in [0,86400000]");
    return static_cast<btc::maker_backtest::Millis>(value);
}
}
int main(int argc, char** argv) {
    using namespace btc::maker_backtest;
    try {
        Config config;
        std::string events = "../data/binance/maker/2023-07-20/events-spot-trades.csv";
        std::string output = "results/binance-maker-spot-fee";
        for (int i = 1; i < argc; ++i) {
            const std::string key = argv[i];
            if (key == "--help") {
                std::cout << "Binance BTCUSDT maker replay (simulated orders only).\n"
                    << "Prepare: python3 tools/download_binance_maker.py [--date 2023-07-20 --hours 1]\n"
                    << "  --events FILE --output DIR\n"
                    << "  --quote-rule spot-fee|cost-budget   default: spot +/- maker fee, basis gate only\n"
                    << "  --maker-fee-bps 2          each fill; example regular-user fee, no BNB discount\n"
                    << "  --max-funding-bps 1        cost-budget only: absolute rate PER SETTLEMENT\n"
                    << "  --min-basis-bps -5 --max-basis-bps 5   perp midpoint vs spot; also caps quote prices\n"
                    << "  --horizon-minutes 5        cost-budget only: funding horizon (not a forced exit)\n"
                    << "  --risk-buffer-bps 0 --profit-bps 0 --max-spread-bps 20   (cost-budget rule)\n"
                    << "  --order-btc 0.01 --max-inventory-btc 0.05 --skew-bps 0   per 0.01 BTC (cost-budget)\n"
                    << "  --tick 0.1 --quantity-step 0.001 --min-notional 100    assumed filters\n"
                    << "  --latency-ms 0 --reprice-ms 0\n"
                    << "  --participation 0.1        max fraction of each eligible historical trade\n"
                    << "  --fill-at-touch 0          default requires trade strictly through quote\n"
                    << "  --max-spot-age-ms 90000 --max-book-age-ms 10000\n";
                return 0;
            }
            if (++i == argc) throw std::runtime_error("Missing value for "+key);
            const std::string value = argv[i];
            auto& p = config.pricing;
            if (key == "--events") events = value;
            else if (key == "--output") output = value;
            else if (key == "--quote-rule") {
                if (value == "spot-fee") p.rule = btc::maker::QuoteRule::SpotFee;
                else if (value == "cost-budget") p.rule = btc::maker::QuoteRule::CostBudget;
                else throw std::runtime_error("quote-rule must be spot-fee or cost-budget");
            }
            else if (key == "--maker-fee-bps") p.maker_fee_bps = number(value);
            else if (key == "--max-funding-bps") p.max_abs_funding_bps = number(value);
            else if (key == "--min-basis-bps") p.min_basis_bps = number(value);
            else if (key == "--max-basis-bps") p.max_basis_bps = number(value);
            else if (key == "--horizon-minutes") p.horizon_minutes = number(value);
            else if (key == "--risk-buffer-bps") p.risk_buffer_bps = number(value);
            else if (key == "--profit-bps") p.profit_bps = number(value);
            else if (key == "--max-spread-bps") p.max_spread_bps = number(value);
            else if (key == "--order-btc") config.order_btc = number(value);
            else if (key == "--max-inventory-btc") config.max_inventory_btc = number(value);
            else if (key == "--skew-bps") p.skew_bps_per_contract = number(value);
            else if (key == "--tick") p.tick = number(value);
            else if (key == "--quantity-step") config.quantity_step = number(value);
            else if (key == "--min-notional") config.min_notional = number(value);
            else if (key == "--participation") config.participation = number(value);
            else if (key == "--latency-ms") config.latency_ms = milliseconds(value);
            else if (key == "--reprice-ms") config.reprice_ms = milliseconds(value);
            else if (key == "--max-spot-age-ms") config.max_spot_age_ms = milliseconds(value);
            else if (key == "--max-book-age-ms") config.max_book_age_ms = milliseconds(value);
            else if (key == "--fill-at-touch") {
                if (value != "0" && value != "1") throw std::runtime_error("fill-at-touch must be 0 or 1");
                config.fill_at_touch = value == "1";
            } else throw std::runtime_error("Unknown option: "+key);
        }
        if (config.pricing.rule == btc::maker::QuoteRule::SpotFee &&
            (config.pricing.risk_buffer_bps != 0 || config.pricing.profit_bps != 0 || config.pricing.skew_bps_per_contract != 0))
            throw std::runtime_error("Risk/profit/skew adjustments require --quote-rule cost-budget; spot-fee quotes exactly spot +/- maker fee");
        Engine engine(config);
        replay(engine,events);
        write_results(engine,output);
        const auto total = engine.totals();
        std::cout << "Binance maker: fills=" << engine.fills().size() << " net_usdt=" << total.net
                  << " fees=" << total.fees << " funding=" << total.funding << " inventory_btc=" << total.inventory
                  << " max_drawdown=" << engine.drawdown() << "\nResults in " << output << '\n';
        return 0;
    } catch (const std::exception& error) { std::cerr << "error: " << error.what() << '\n'; return 1; }
}
