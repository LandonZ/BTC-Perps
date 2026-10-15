#include "basis-backtest.h"
#include "../model/theo-model.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace btc::backtest {
namespace {
void require(bool okay, const std::string& message) { if (!okay) throw std::runtime_error(message); }
std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> cells;
    std::string cell;
    bool quote = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char ch = line[i];
        if (ch == '"') {
            if (quote && i+1 < line.size() && line[i+1] == '"') { cell += '"'; ++i; }
            else quote = !quote;
        } else if (ch == ',' && !quote) { cells.push_back(cell); cell.clear(); }
        else if (ch != '\r') cell += ch;
    }
    require(!quote,"Unclosed CSV quote");
    cells.push_back(cell);
    return cells;
}
struct Csv {
    std::ifstream file;
    std::map<std::string,std::size_t> columns;
    std::vector<std::string> row;
    explicit Csv(const std::string& path):file(path) {
        require(file.good(),"Cannot read " + path + "; download price history first");
        std::string line;
        require(bool(std::getline(file,line)),"Empty CSV: " + path);
        const auto names = split(line);
        for (std::size_t i = 0; i < names.size(); ++i) columns[names[i]] = i;
    }
    bool next() {
        std::string line;
        if (!std::getline(file,line)) return false;
        row = split(line);
        return true;
    }
    const std::string& cell(const std::string& name) const {
        const auto col = columns.find(name);
        require(col != columns.end() && col->second < row.size(),"Missing CSV column " + name);
        return row[col->second];
    }
    double number(const std::string& name) const {
        const auto& value = cell(name);
        std::size_t used = 0;
        const double result = std::stod(value,&used);
        require(used == value.size() && std::isfinite(result),"Invalid CSV number " + name);
        return result;
    }
    Time time() const {
        const auto& value = cell("ts");
        std::size_t used = 0;
        const Time result = std::stoll(value,&used);
        require(used == value.size(),"Invalid CSV timestamp");
        return result;
    }
};
struct Candle { double open,close; };
std::map<Time,Candle> candles(const std::string& path) {
    Csv csv(path);
    std::map<Time,Candle> result;
    while (csv.next()) {
        const auto t = csv.time();
        const double open = csv.number("open"),close = csv.number("close");
        require(t > 0 && t%60 == 0 && open > 0 && close > 0,"Invalid minute candle");
        require(result.emplace(t,Candle{open,close}).second,"Duplicate candle timestamp");
    }
    require(!result.empty(),"No candles in " + path);
    return result;
}
std::optional<double> trailing_basis(const Dataset& data,Time now) {
    double sum = 0;
    for (Time t = now-3600; t < now; t += 60) {
        const auto row = data.prices.find(t);
        if (row == data.prices.end()) return {};
        sum += (row->second.perp_close/row->second.spot_close-1)*theo::bps;
    }
    return sum/60;
}
const char* side(int direction) { return direction > 0 ? "LONG_PERP_SHORT_SPOT" : "SHORT_PERP_LONG_SPOT"; }
std::ofstream output(const std::filesystem::path& path) {
    std::ofstream file(path);
    require(file.good(),"Cannot write " + path.string());
    file << std::fixed << std::setprecision(10);
    return file;
}
} // namespace

std::string utc(Time value) {
    const std::time_t t = static_cast<std::time_t>(value);
    std::tm tm{};
    require(gmtime_r(&t,&tm) != nullptr,"Invalid UTC timestamp");
    char text[32];
    require(std::strftime(text,sizeof(text),"%Y-%m-%dT%H:%M:%SZ",&tm) != 0,"UTC formatting failed");
    return text;
}
const char* strategy_name(Strategy strategy) {
    switch (strategy) {
        case Strategy::BothBasis: return "both_basis_threshold";
        case Strategy::LongBasis: return "long_basis_threshold";
        case Strategy::ShortBasis: return "short_basis_threshold";
        case Strategy::TheoCarry: return "theo_carry";
    }
    throw std::runtime_error("Unknown strategy");
}
Dataset load(const std::string& spot_file,const std::string& perp_file,const std::string& funding_file) {
    const auto spot = candles(spot_file),perp = candles(perp_file);
    Dataset data;
    data.spot_rows = spot.size(); data.perp_rows = perp.size();
    Csv csv(funding_file);
    while (csv.next()) {
        Funding f{csv.time(),csv.number("funding_rate"),csv.number("mark_price")};
        require(f.time > 0 && f.time%3600 == 0 && std::abs(f.rate) < 1 && f.mark > 0,"Invalid hourly funding row");
        require(data.funding.empty() || f.time-data.funding.back().time == 3600,
                "Funding history must be sorted and hourly without gaps; missing rates cannot be zero-filled");
        data.funding.push_back(f);
    }
    require(!data.funding.empty(),"No funding history");
    for (const auto& [t,s] : spot) {
        const auto p = perp.find(t);
        if (p != perp.end() && t >= data.funding.front().time && t <= data.funding.back().time)
            data.prices.emplace(t,Prices{s.open,p->second.open,s.close,p->second.close});
    }
    require(data.prices.size() > 60,"Insufficient overlapping prices and funding");
    return data;
}
namespace {
Result run_threshold_basis(const Dataset& data,const Settings& settings) {
    Result result;
    const Time first = data.prices.begin()->first,last = data.prices.rbegin()->first;
    result.missing_minutes = static_cast<std::size_t>((last-first)/60+1)-data.prices.size();
    std::optional<Trade> position;
    std::size_t funding_index = 0;
    for (const auto& [t,p] : data.prices) {
        require(t%60 == 0 && std::isfinite(p.spot_open) && p.spot_open > 0 &&
                std::isfinite(p.perp_open) && p.perp_open > 0 &&
                std::isfinite(p.spot_close) && p.spot_close > 0 &&
                std::isfinite(p.perp_close) && p.perp_close > 0,"Invalid joined price");
        while (funding_index < data.funding.size() && data.funding[funding_index].time <= t) {
            const auto& f = data.funding[funding_index++];
            if (position && f.time > position->entry) {
                position->funding_pnl -= position->direction*settings.quantity_btc*f.mark*f.rate;
                ++position->funding_payments;
            }
        }
        const double basis = (p.perp_open/p.spot_open-1)*theo::bps;
        bool closed_this_minute = false;
        if (position) {
            auto& tr = *position;
            const Time deadline = tr.entry+settings.holding_minutes*60LL;
            // If the deadline quote is absent, close at the first quote observed after it.
            // Dropping the trade instead would select the sample on future data availability.
            const bool late = t > deadline;
            result.delayed_exits += late;
            const double price = tr.direction*settings.quantity_btc*((p.perp_open-tr.entry_perp)-(p.spot_open-tr.entry_spot));
            const bool profit = settings.exit_on_profit && price+tr.funding_pnl > 1e-10;
            const bool converged = tr.direction > 0 ? basis >= -settings.discount_threshold_bps-1e-10 :
                                                     basis <= settings.discount_threshold_bps+1e-10;
            if (profit || converged || t >= deadline || t == last) {
                tr.exit = t; tr.exit_spot = p.spot_open; tr.exit_perp = p.perp_open;
                tr.exit_basis_bps = basis; tr.price_pnl = price; tr.net_pnl = price+tr.funding_pnl;
                tr.net_bps = tr.net_pnl/(settings.quantity_btc*tr.entry_spot)*theo::bps;
                tr.forced_end = !profit && !converged && t < deadline;
                tr.exit_reason = late ? "LATE_TIMEOUT" : profit ? "PROFIT" : converged ? "CONVERGED" : t == deadline ? "TIMEOUT" : "DATA_END";
                result.price_pnl += tr.price_pnl; result.funding_pnl += tr.funding_pnl;
                result.mean_trade_bps += tr.net_bps; result.wins += tr.net_pnl > 0;
                result.long_trades += tr.direction > 0;
                result.short_trades += tr.direction < 0;
                result.trades.push_back(tr);
                position.reset(); closed_this_minute = true;
            }
        }
        // Entry checks are every observed minute. Never close and reopen in the same minute.
        int entry_direction = 0;
        if (basis < -settings.discount_threshold_bps-1e-10 && settings.strategy != Strategy::ShortBasis) entry_direction = 1;
        else if (basis > settings.discount_threshold_bps+1e-10 && settings.strategy != Strategy::LongBasis) entry_direction = -1;
        if (!position && !closed_this_minute && t != last && entry_direction) {
            const auto average = trailing_basis(data,t);
            if (!average || funding_index == 0) { ++result.skipped_feature_hours; continue; }
            theo::BasisInputs in;
            in.index = p.spot_open; in.spot_bid = in.spot_ask = p.spot_open;
            in.perp_bid = in.perp_ask = in.last_trade = p.perp_open;
            in.twap_basis_bps = *average;
            in.previous_funding_bps = data.funding[funding_index-1].rate*theo::bps;
            in.exit_basis_bps = settings.forecast_exit_basis_bps.value_or(-entry_direction*settings.discount_threshold_bps);
            in.horizon_minutes = settings.holding_minutes;
            in.minutes_to_next_funding = (3600-t%3600)/60.0;
            theo::BasisCosts costs;
            costs.quantity_btc = settings.quantity_btc;
            const auto forecast = theo::basis_theo(in,costs);
            const double edge = entry_direction > 0 ? forecast.long_net_pnl_bps : forecast.short_net_pnl_bps;
            if (edge <= settings.minimum_edge_bps) continue;
            Trade tr;
            tr.entry = t; tr.direction = entry_direction; tr.entry_spot = p.spot_open; tr.entry_perp = p.perp_open;
            tr.entry_basis_bps = basis; tr.trailing_basis_bps = *average;
            tr.previous_funding_bps = in.previous_funding_bps;
            tr.forecast_funding_bps = forecast.long_funding_paid_bps;
            tr.forecast_edge_bps = edge;
            position = tr;
        }
    }
    require(!position,"Final position must close");
    result.net_pnl = result.price_pnl+result.funding_pnl;
    if (!result.trades.empty()) result.mean_trade_bps /= result.trades.size();

    // Reconstruct equity from the closed trades.
    std::vector<double> funding_prefix(1,0);
    for (const auto& f : data.funding) funding_prefix.push_back(funding_prefix.back()+f.mark*f.rate);
    std::size_t trade_index = 0,event_index = 0;
    double closed_price = 0,closed_funding = 0,peak = 0;
    for (const auto& [t,p] : data.prices) {
        while (event_index < data.funding.size() && data.funding[event_index].time <= t) ++event_index;
        while (trade_index < result.trades.size() && result.trades[trade_index].exit < t) {
            closed_price += result.trades[trade_index].price_pnl;
            closed_funding += result.trades[trade_index].funding_pnl;
            ++trade_index;
        }
        double price = closed_price,funding = closed_funding;
        int direction = 0;
        if (trade_index < result.trades.size() && result.trades[trade_index].entry <= t) {
            const auto& tr = result.trades[trade_index];
            const auto entry_events = std::upper_bound(data.funding.begin(),data.funding.end(),tr.entry,
                [](Time time,const Funding& event) { return time < event.time; });
            const auto entry_index = static_cast<std::size_t>(entry_events-data.funding.begin());
            price += tr.direction*settings.quantity_btc*((p.perp_open-tr.entry_perp)-(p.spot_open-tr.entry_spot));
            funding -= tr.direction*settings.quantity_btc*(funding_prefix[event_index]-funding_prefix[entry_index]);
            if (t < tr.exit) direction = tr.direction;
        }
        const double net = price+funding;
        peak = std::max(peak,net); result.max_drawdown = std::max(result.max_drawdown,peak-net);
        result.equity.push_back({t,price,funding,net,direction});
    }
    return result;
}
} // namespace
Result run(const Dataset& data,const Settings& settings) {
    require(settings.holding_minutes > 0 && settings.holding_minutes <= 525600,"Invalid holding period");
    require(std::isfinite(settings.quantity_btc) && settings.quantity_btc > 0,"Quantity must be positive");
    require(std::isfinite(settings.minimum_edge_bps) && settings.minimum_edge_bps >= 0,"Invalid minimum edge");
    require(std::isfinite(settings.discount_threshold_bps) && settings.discount_threshold_bps >= 0,
            "Invalid discount threshold");
    if (settings.strategy != Strategy::TheoCarry) require(settings.holding_minutes <= 5,"Basis threshold maximum holding period is five minutes");
    if (settings.forecast_exit_basis_bps)
        require(std::isfinite(*settings.forecast_exit_basis_bps) && *settings.forecast_exit_basis_bps > -theo::bps,
                "Invalid forecast exit basis");
    require(!data.prices.empty() && !data.funding.empty(),"Empty backtest dataset");
    const Time first = data.prices.begin()->first,last = data.prices.rbegin()->first;
    require(first >= data.funding.front().time && last <= data.funding.back().time,"Prices exceed funding coverage");
    for (std::size_t i = 0; i < data.funding.size(); ++i) {
        const auto& f = data.funding[i];
        require(f.time%3600 == 0 && std::isfinite(f.rate) && std::abs(f.rate) < 1 &&
                std::isfinite(f.mark) && f.mark > 0,"Invalid funding history");
        if (i) require(f.time-data.funding[i-1].time == 3600,"Missing/duplicate funding boundary");
    }
    if (settings.strategy != Strategy::TheoCarry) return run_threshold_basis(data,settings);
    Result result;
    result.missing_minutes = static_cast<std::size_t>((last-first)/60+1)-data.prices.size();
    std::optional<Trade> position;
    std::size_t funding_index = 0;
    double closed_price = 0,closed_funding = 0,peak = 0;
    for (const auto& [t,p] : data.prices) {
        require(t%60 == 0 && std::isfinite(p.spot_open) && p.spot_open > 0 &&
                std::isfinite(p.perp_open) && p.perp_open > 0 &&
                std::isfinite(p.spot_close) && p.spot_close > 0 &&
                std::isfinite(p.perp_close) && p.perp_close > 0,"Invalid joined price");
        // Process funding BEFORE exits and entries: ownership convention is (entry, exit].
        while (funding_index < data.funding.size() && data.funding[funding_index].time <= t) {
            const auto& f = data.funding[funding_index++];
            if (position && f.time > position->entry) {
                position->funding_pnl -= position->direction*settings.quantity_btc*f.mark*f.rate;
                ++position->funding_payments;
            }
        }
        if (position && (t >= position->entry+settings.holding_minutes*60LL || t == last)) {
            auto& tr = *position;
            tr.exit = t; tr.exit_spot = p.spot_open; tr.exit_perp = p.perp_open;
            tr.exit_basis_bps = (p.perp_open/p.spot_open-1)*theo::bps;
            // Equal BTC quantities: exact dollar PnL, allowing spot to move.
            tr.price_pnl = tr.direction*settings.quantity_btc*
                ((tr.exit_perp-tr.entry_perp)-(tr.exit_spot-tr.entry_spot));
            tr.net_pnl = tr.price_pnl+tr.funding_pnl;
            tr.net_bps = tr.net_pnl/(settings.quantity_btc*tr.entry_spot)*theo::bps;
            tr.forced_end = t < tr.entry+settings.holding_minutes*60LL;
            tr.exit_reason = tr.forced_end ? "DATA_END" : "HORIZON";
            if (t > tr.entry+settings.holding_minutes*60LL) ++result.delayed_exits;
            closed_price += tr.price_pnl; closed_funding += tr.funding_pnl;
            result.wins += tr.net_pnl > 0;
            result.long_trades += tr.direction > 0;
            result.short_trades += tr.direction < 0;
            result.mean_trade_bps += tr.net_bps;
            result.trades.push_back(tr);
            position.reset();
        }
        if (!position && t%3600 == 0 && t != last) {
            const auto average = trailing_basis(data,t);
            if (!average || funding_index == 0) ++result.skipped_feature_hours;
            else {
                theo::BasisInputs in;
                in.index = p.spot_open;
                in.spot_bid = in.spot_ask = p.spot_open;
                in.perp_bid = in.perp_ask = in.last_trade = p.perp_open;
                in.twap_basis_bps = *average;
                in.previous_funding_bps = data.funding[funding_index-1].rate*theo::bps;
                in.exit_basis_bps = settings.forecast_exit_basis_bps.value_or((p.perp_open/p.spot_open-1)*theo::bps);
                in.horizon_minutes = settings.holding_minutes; in.minutes_to_next_funding = 60;
                theo::BasisCosts costs;
                costs.quantity_btc = settings.quantity_btc; costs.min_net_edge_bps = settings.minimum_edge_bps;
                const auto forecast = theo::basis_theo(in,costs);
                int direction = 0;
                if (std::string(forecast.candidate) == "LONG_PERP_SHORT_SPOT") direction = 1;
                else if (std::string(forecast.candidate) == "SHORT_PERP_LONG_SPOT") direction = -1;
                if (direction) {
                    Trade tr;
                    tr.entry = t; tr.direction = direction;
                    tr.entry_spot = p.spot_open; tr.entry_perp = p.perp_open;
                    tr.entry_basis_bps = forecast.market_basis_bps;
                    tr.trailing_basis_bps = *average; tr.previous_funding_bps = in.previous_funding_bps;
                    tr.forecast_funding_bps = forecast.long_funding_paid_bps;
                    tr.forecast_edge_bps = direction > 0 ? forecast.long_net_pnl_bps : forecast.short_net_pnl_bps;
                    position = tr;
                }
            }
        }
        double price = closed_price,funding = closed_funding;
        if (position) {
            price += position->direction*settings.quantity_btc*
                ((p.perp_open-position->entry_perp)-(p.spot_open-position->entry_spot));
            funding += position->funding_pnl;
        }
        const double net = price+funding;
        peak = std::max(peak,net); result.max_drawdown = std::max(result.max_drawdown,peak-net);
        result.equity.push_back(Equity{t,price,funding,net,position ? position->direction : 0});
    }
    require(!position,"Position not closed at end of dataset");
    result.price_pnl = closed_price; result.funding_pnl = closed_funding;
    result.net_pnl = closed_price+closed_funding;
    if (!result.trades.empty()) result.mean_trade_bps /= result.trades.size();
    return result;
}

void write_results(const Dataset& data,const Settings& s,const Result& r,const std::string& directory) {
    const std::filesystem::path dir(directory);
    std::filesystem::create_directories(dir);
    auto trades = output(dir/"trades.csv");
    trades << "entry_ts,exit_ts,entry_utc,exit_utc,side,quantity_btc,entry_spot,entry_perp,exit_spot,exit_perp,"
              "entry_basis_bps,exit_basis_bps,trailing_basis_bps,previous_funding_bps,forecast_funding_bps,"
              "forecast_edge_bps,price_pnl,funding_pnl,net_pnl,net_bps,funding_payments,forced_end,exit_reason\n";
    for (const auto& tr : r.trades)
        trades << tr.entry << ',' << tr.exit << ',' << utc(tr.entry) << ',' << utc(tr.exit) << ',' << side(tr.direction)
               << ',' << s.quantity_btc << ',' << tr.entry_spot << ',' << tr.entry_perp << ',' << tr.exit_spot << ','
               << tr.exit_perp << ',' << tr.entry_basis_bps << ',' << tr.exit_basis_bps << ',' << tr.trailing_basis_bps
               << ',' << tr.previous_funding_bps << ',' << tr.forecast_funding_bps << ',' << tr.forecast_edge_bps
               << ',' << tr.price_pnl << ',' << tr.funding_pnl << ',' << tr.net_pnl << ',' << tr.net_bps << ','
               << tr.funding_payments << ',' << tr.forced_end << ',' << tr.exit_reason << '\n';
    auto equity = output(dir/"equity.csv");
    equity << "ts,utc,price_pnl,funding_pnl,net_pnl,direction\n";
    for (const auto& e : r.equity)
        equity << e.time << ',' << utc(e.time) << ',' << e.price_pnl << ',' << e.funding_pnl << ','
               << e.net_pnl << ',' << e.direction << '\n';
    auto summary = output(dir/"summary.json");
    summary << "{\n  \"start_utc\": \"" << utc(data.prices.begin()->first) << "\",\n"
            << "  \"end_utc\": \"" << utc(data.prices.rbegin()->first) << "\",\n"
            << "  \"holding_minutes\": " << s.holding_minutes << ",\n"
            << "  \"strategy\": \"" << strategy_name(s.strategy) << "\",\n"
            << "  \"discount_threshold_bps\": " << s.discount_threshold_bps << ",\n"
            << "  \"exit_on_profit\": " << (s.exit_on_profit ? "true" : "false") << ",\n"
            << "  \"quantity_btc\": " << s.quantity_btc << ",\n  \"cost_bps\": 0,\n"
            << "  \"minimum_edge_bps\": " << s.minimum_edge_bps << ",\n"
            << "  \"exit_basis_forecast\": " << (s.forecast_exit_basis_bps ? std::to_string(*s.forecast_exit_basis_bps) :
                s.strategy == Strategy::BothBasis ? "\"symmetric_threshold_targets\"" :
                s.strategy == Strategy::LongBasis ? std::to_string(-s.discount_threshold_bps) :
                s.strategy == Strategy::ShortBasis ? std::to_string(s.discount_threshold_bps) : "\"persist_entry_basis\"") << ",\n"
            << "  \"funding_pnl_source\": \"recorded_hourly_rates_and_marks\",\n"
            << "  \"spot_rows\": " << data.spot_rows << ",\n  \"perp_rows\": " << data.perp_rows << ",\n"
            << "  \"matched_rows\": " << data.prices.size() << ",\n"
            << "  \"missing_matched_minutes\": " << r.missing_minutes << ",\n"
            << "  \"skipped_feature_hours\": " << r.skipped_feature_hours << ",\n"
            << "  \"skipped_feature_checks\": " << r.skipped_feature_hours << ",\n"
            << "  \"delayed_exits\": " << r.delayed_exits << ",\n"
            << "  \"trades\": " << r.trades.size() << ",\n  \"long_trades\": " << r.long_trades << ",\n"
            << "  \"short_trades\": " << r.short_trades << ",\n  \"winning_trades\": " << r.wins << ",\n"
            << "  \"price_pnl\": " << r.price_pnl << ",\n  \"funding_pnl\": " << r.funding_pnl << ",\n"
            << "  \"net_pnl\": " << r.net_pnl << ",\n  \"max_drawdown\": " << r.max_drawdown << ",\n"
            << "  \"mean_trade_bps\": " << r.mean_trade_bps << "\n}\n";
    require(trades.good() && equity.good() && summary.good(),"Failed to write complete results");
}
} // namespace btc::backtest
