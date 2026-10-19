#include "primary-model.h"
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

namespace {
using namespace btc::directional;
double number(const std::string& text) {
    std::size_t n = 0; const double v = std::stod(text,&n);
    if (n != text.size() || !std::isfinite(v)) throw std::runtime_error("Invalid numeric argument: " + text);
    return v;
}
int horizon_value(double value) {
    if (value < 1 || value > 1440 || std::floor(value) != value) throw std::runtime_error("Horizon must be an integer from 1 to 1440 minutes");
    return static_cast<int>(value);
}
std::string utc(Timestamp ts) {
    const auto t = static_cast<std::time_t>(ts); const auto* calendar = std::gmtime(&t);
    if (!calendar) throw std::runtime_error("Cannot format timestamp");
    char buffer[32]; std::strftime(buffer,sizeof(buffer),"%Y-%m-%dT%H:%M:%SZ",calendar); return buffer;
}
void metric_line(const std::string& name,const Metrics& m) {
    std::cout << name << ": N=" << m.observations << ", MAE=" << m.mae_bps
        << " bps, RMSE=" << m.rmse_bps << " bps; unchanged-price MAE=" << m.no_change_mae_bps
        << ", RMSE=" << m.no_change_rmse_bps << " bps\n";
}
void write_metrics(std::ostream& out,const Metrics& m) {
    out << "{\"observations\":" << m.observations << ",\"mae_bps\":" << m.mae_bps
        << ",\"rmse_bps\":" << m.rmse_bps << ",\"unchanged_price_mae_bps\":" << m.no_change_mae_bps
        << ",\"unchanged_price_rmse_bps\":" << m.no_change_rmse_bps << '}';
}
void help() {
    std::cout << "C++ BTC perp future-price forecast and unhedged long/short PnL\n"
        << "Train: --train --spot-csv FILE --perp-csv FILE --funding-csv FILE\n"
        << "       --horizon-minutes 5 --ridge 0.1 --model FILE\n"
        << "Predict: --latest (historical CSV snapshot), or supply current features:\n"
        << "  --perp PRICE --spot PRICE --perp-return-5m-bps X --spot-return-5m-bps X\n"
        << "  --volatility-5m-bps X --hourly-funding-bps X --minutes-to-funding 60\n"
        << "Fees REQUIRED for PnL: --entry-fee-bps X --exit-fee-bps Y\n"
        << "  Or --total-fee-bps X (splits the supplied rate equally across entry/exit).\n"
        << "Costs: --half-spread-bps 1 --slippage-bps 0.5 --financing-apr 0.05\n"
        << "       --margin 0.2 --qty-btc 0.01 --min-edge-bps 1\n"
        << "Funding override: --funding-paid-bps X (cumulative long debit; negative = receipt).\n"
        << "What-if forecast: --expected-price PRICE --horizon-minutes 5 --perp PRICE --spot PRICE\n"
        << "Without supplied fees, only the price forecast is shown; no trade direction is emitted.\n"
        << "Hourly data/funding mode is legacy INTX; modern continuous funding needs the override.\n";
}
}

int main(int argc,char** argv) {
    using namespace btc::directional;
    try {
        bool training = false,latest = false,horizon_supplied = false;
        int horizon = 5; double ridge = 0.1,quantity = 0.01;
        Snapshot s; Costs costs;
        std::optional<double> entry_fee,exit_fee,total_fee,funding_override,expected_price;
        std::string model_path = "Primary-Market-Model/build/directional.model";
        std::string spot_path = "data/coinbase/BTC-USD_spot_1m.csv";
        std::string perp_path = "data/coinbase/BTC-PERP-INTX_1m.csv";
        std::string funding_path = "data/coinbase/BTC-PERP-INTX_funding_1h.csv";
        for (int i = 1; i < argc; ++i) {
            const std::string key = argv[i];
            if (key == "--help") { help(); return 0; }
            if (key == "--train") { training = true; continue; }
            if (key == "--latest") { latest = true; continue; }
            if (++i >= argc) throw std::runtime_error("Missing value for " + key);
            const std::string value = argv[i];
            if (key == "--model") model_path = value;
            else if (key == "--spot-csv") spot_path = value;
            else if (key == "--perp-csv") perp_path = value;
            else if (key == "--funding-csv") funding_path = value;
            else {
                const double v = number(value);
                if (key == "--horizon-minutes") { horizon = horizon_value(v); horizon_supplied = true; }
                else if (key == "--ridge") ridge = v;
                else if (key == "--qty-btc") quantity = v;
                else if (key == "--perp") s.perp = v;
                else if (key == "--spot") s.spot = v;
                else if (key == "--perp-return-5m-bps") s.perp_return_5m_bps = v;
                else if (key == "--spot-return-5m-bps") s.spot_return_5m_bps = v;
                else if (key == "--volatility-5m-bps") s.perp_volatility_5m_bps = v;
                else if (key == "--hourly-funding-bps") s.known_hourly_funding_bps = v;
                else if (key == "--minutes-to-funding") s.minutes_to_next_funding = v;
                else if (key == "--entry-fee-bps") entry_fee = v;
                else if (key == "--exit-fee-bps") exit_fee = v;
                else if (key == "--total-fee-bps") total_fee = v;
                else if (key == "--funding-paid-bps") funding_override = v;
                else if (key == "--expected-price") expected_price = v;
                else if (key == "--half-spread-bps") costs.half_spread_bps = v;
                else if (key == "--slippage-bps") costs.slippage_bps_per_fill = v;
                else if (key == "--financing-apr") costs.financing_apr = v;
                else if (key == "--margin") costs.margin_fraction = v;
                else if (key == "--min-edge-bps") costs.min_net_edge_bps = v;
                else throw std::runtime_error("Unknown argument: " + key);
            }
        }
        if (total_fee && (entry_fee || exit_fee)) throw std::runtime_error("Supply total fees OR entry/exit fees, not both");
        if (entry_fee.has_value() != exit_fee.has_value()) throw std::runtime_error("Supply both entry and exit fees");
        if (training && expected_price) throw std::runtime_error("A supplied what-if price cannot be used to train a regression");
        if (total_fee) entry_fee = exit_fee = *total_fee/2;
        if (!training && !latest && s.perp == 0) { help(); return 1; }
        std::optional<Dataset> data;
        if (training || latest) data = load_dataset(spot_path,perp_path,funding_path);
        ForecastModel model;
        if (training) {
            const auto examples = make_examples(*data,horizon);
            const auto first = data->points.front().ts,last = data->points.back().ts;
            const Timestamp validation_start = ((first+(last-first)*7/10+59)/60)*60;
            const Timestamp test_start = ((first+(last-first)*85/100+59)/60)*60;
            std::vector<Example> train,validation,test;
            for (const auto& e : examples) {
                if (e.end_ts < validation_start) train.push_back(e);
                else if (e.ts >= validation_start && e.end_ts < test_start) validation.push_back(e);
                else if (e.ts >= test_start) test.push_back(e);
            }
            model = fit(train,horizon,ridge);
            model.validation = evaluate(model,validation);
            const auto train_metrics = evaluate(model,train),test_metrics = evaluate(model,test);
            model.save(model_path);
            std::ofstream report(model_path+".metrics.json");
            if (!report) throw std::runtime_error("Cannot write forecast metrics");
            report.exceptions(std::ios::badbit|std::ios::failbit); report << std::setprecision(12);
            report << "{\n\"instrument\":\"legacy Coinbase BTC-PERP-INTX\",\n"
                << "\"target\":\"future unhedged perp return in bps\",\n"
                << "\"horizon_minutes\":" << horizon << ",\n\"ridge\":" << ridge
                << ",\n\"validation_start_utc\":\"" << utc(validation_start) << "\",\n"
                << "\"test_start_utc\":\"" << utc(test_start) << "\",\n"
                << "\"latest_data_utc\":\"" << utc(last) << "\",\n\"train\":";
            write_metrics(report,train_metrics); report << ",\n\"validation\":";
            write_metrics(report,model.validation); report << ",\n\"test\":";
            write_metrics(report,test_metrics); report << "\n}\n";
            std::cout << std::fixed << std::setprecision(4)
                << "Fitted " << horizon << "-minute perp-return ridge regression. No spot position.\n";
            metric_line("Train",train_metrics); metric_line("Validation",model.validation); metric_line("Test",test_metrics);
            std::cout << "Saved coefficients: " << model_path << "\n"
                << "Metrics use chronological 70/15/15 periods; targets crossing boundaries are excluded.\n"
                << "This measures forecast error, not realized trading profitability.\n\n";
            if (!latest) return 0;
        } else if (!expected_price) {
            model = ForecastModel::load(model_path);
            if (horizon_supplied && horizon != model.horizon_minutes)
                throw std::runtime_error("Prediction horizon must match the fitted model");
            horizon = model.horizon_minutes;
        }
        if (latest) {
            bool found = false;
            for (std::size_t i = data->points.size(); i-- > 5;) {
                try { s = snapshot_at(*data,i); found = true; break; }
                catch (const std::runtime_error&) { /* Skip incomplete trailing feature windows. */ }
            }
            if (!found) throw std::runtime_error("No complete latest feature window");
            std::cout << "Historical snapshot (not live): " << utc(s.ts) << '\n';
        }
        const auto x = features(s,horizon);
        const double predicted_return = expected_price ? (*expected_price/s.perp-1)*bps : model.predict_return_bps(x);
        const double predicted = expected_price ? *expected_price : s.perp*(1+predicted_return/bps);
        if (predicted <= 0) throw std::runtime_error("Regression extrapolated to a nonpositive price");
        const double funding = forecast_funding_per_btc(s,predicted,horizon,funding_override);
        std::cout << std::fixed << std::setprecision(6)
            << "Forecast source:       " << (expected_price ? "supplied what-if price" : "fitted return regression") << '\n'
            << "Horizon:               " << horizon << " minutes\n"
            << "Current perp mid:      " << s.perp << '\n'
            << "PREDICTED PERP MID:    " << predicted << '\n'
            << "Predicted return:      " << predicted_return << " bps\n"
            << "Long funding paid/BTC: " << funding << '\n'
            << "Funding assumption:    " << (funding_override ? "supplied cumulative horizon forecast" : "persisted legacy hourly rate") << '\n';
        if (!expected_price) {
            std::cout << "Training error RMSE:   " << model.training_rmse_bps << " bps\n";
            if (model.validation.mae_bps >= model.validation.no_change_mae_bps)
                std::cout << "Validation: forecast did not beat unchanged price on MAE.\n";
        }
        if (!entry_fee) {
            std::cout << "\nPerp-only fees are missing. Expected net PnL and LONG/SHORT decisions are withheld.\n"
                << "Supply --entry-fee-bps and --exit-fee-bps, or --total-fee-bps.\n";
            return 0;
        }
        costs.entry_fee_bps = *entry_fee; costs.exit_fee_bps = *exit_fee;
        const auto p = profits(s,predicted,funding,quantity,horizon,costs);
        std::cout << "Quantity:              " << quantity << " BTC\n"
            << "Entry/exit fee rates:  " << costs.entry_fee_bps << '/' << costs.exit_fee_bps << " bps\n"
            << "Long total fees:       " << p.long_fees << '\n'
            << "Short total fees:      " << p.short_fees << '\n'
            << "Margin financing:      " << p.financing << '\n'
            << "EXPECTED LONG PnL:     " << p.long_pnl << " (" << p.long_edge_bps << " bps)\n"
            << "EXPECTED SHORT PnL:    " << p.short_pnl << " (" << p.short_edge_bps << " bps)\n"
            << "MODEL CANDIDATE:       " << p.decision << '\n'
            << "\nExpected PnL uses adverse entry/exit prices, signed funding, fees and financing.\n"
            << "It is conditional on the forecast, not a guarantee or an executed trade.\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "Error: " << e.what() << '\n'; return 1; }
}
