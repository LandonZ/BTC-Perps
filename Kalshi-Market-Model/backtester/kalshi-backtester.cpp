// Historical backtest of the Primary-Market-Model basis rule on Kalshi KXBTCPERP.
//
// Same rule as Primary-Market-Model/backtester/basis-backtest.cpp, with the
// Kalshi perp in place of BTC-PERP-INTX and Coinbase BTC-USD as spot:
//   basis = 10,000 * (perp / spot - 1), checked at each matched minute open
//   - buy basis  (long perp / short spot) when basis < -threshold
//   - sell basis (short perp / long spot) when basis > +threshold
//   - exit on first positive PnL, convergence to the threshold, or the deadline
//   - one equal-BTC pair at a time; never close and reopen in the same minute
//
// Differences forced by the venue: Kalshi funds every 8h (not hourly), so the
// funding forecast persists the last recorded rate across any boundary in the
// holding window. `--fill mid` (default) prices the perp at the bid/ask midpoint,
// matching the primary run's idealized fills; `--fill touch` buys at the ask and
// sells at the bid, so every round trip pays the Kalshi spread.
#include <cmath>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Time = std::int64_t;
constexpr double BPS = 1e4;
constexpr double CONTRACT_BTC = 0.0001;  // Kalshi quotes per 0.0001 BTC contract
constexpr Time FUNDING_PERIOD = 8 * 3600, FUNDING_OFFSET = 4 * 3600;  // settles 04:00, 12:00, 20:00 UTC

void require(bool okay, const std::string& message) { if (!okay) throw std::runtime_error(message); }

std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> cells;
    std::string cell;
    for (char ch : line) {
        if (ch == ',') { cells.push_back(cell); cell.clear(); }
        else if (ch != '\r') cell += ch;
    }
    cells.push_back(cell);
    return cells;
}

std::optional<double> parse(const std::string& text) {
    if (text.empty()) return {};
    std::size_t used = 0;
    double value;
    try { value = std::stod(text, &used); } catch (const std::exception&) { return {}; }
    if (used != text.size() || !std::isfinite(value)) return {};
    return value;
}

struct Csv {
    std::ifstream file;
    std::map<std::string, std::size_t> columns;
    std::vector<std::string> row;
    explicit Csv(const std::string& path) : file(path) {
        require(file.good(), "Cannot read " + path);
        std::string line;
        require(bool(std::getline(file, line)), "Empty CSV: " + path);
        const auto names = split(line);
        for (std::size_t i = 0; i < names.size(); ++i) columns[names[i]] = i;
    }
    bool next() {
        std::string line;
        if (!std::getline(file, line)) return false;
        row = split(line);
        return true;
    }
    const std::string& cell(const std::string& name) const {
        const auto col = columns.find(name);
        require(col != columns.end() && col->second < row.size(), "Missing CSV column " + name);
        return row[col->second];
    }
    std::optional<double> number(const std::string& name) const { return parse(cell(name)); }
    Time time() const { return static_cast<Time>(std::stoll(cell("ts"))); }
};

std::string utc(Time value) {
    const std::time_t t = static_cast<std::time_t>(value);
    std::tm tm{};
    require(gmtime_r(&t, &tm) != nullptr, "Invalid UTC timestamp");
    char text[32];
    std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return text;
}

// ---------------------------------------------------------------- data

struct Minute { double spot = 0, bid = 0, ask = 0; };  // all USD per BTC, at the minute open
struct Funding { Time time = 0; double rate = 0, mark = 0; };
struct Dataset {
    std::map<Time, Minute> prices;  // keyed by minute START time
    std::vector<Funding> funding;
    std::size_t spot_rows = 0, perp_rows = 0, bad_quotes = 0;
};

Dataset load(const std::string& spot_file, const std::string& perp_file, const std::string& funding_file,
             Time start_time) {
    Dataset data;
    std::map<Time, double> spot;
    {
        Csv csv(spot_file);  // Coinbase: ts is the candle start
        while (csv.next()) {
            const auto open = csv.number("open");
            require(open && *open > 0, "Invalid spot candle");
            spot[csv.time()] = *open;
        }
    }
    data.spot_rows = spot.size();
    {
        Csv csv(perp_file);  // Kalshi: ts is end_period_ts, so the candle starts 60s earlier
        while (csv.next()) {
            ++data.perp_rows;
            const Time start = csv.time() - 60;
            if (start < start_time) continue;
            const auto bid = csv.number("bid_open"), ask = csv.number("ask_open");
            if (!bid || !ask || *bid <= 0 || *ask <= *bid) { ++data.bad_quotes; continue; }
            const auto s = spot.find(start);
            if (s == spot.end()) continue;
            data.prices[start] = Minute{s->second, *bid / CONTRACT_BTC, *ask / CONTRACT_BTC};
        }
    }
    {
        Csv csv(funding_file);
        while (csv.next()) {
            const auto rate = csv.number("funding_rate"), mark = csv.number("mark_price_usd_per_btc");
            require(rate && mark && *mark > 0 && std::abs(*rate) < 1, "Invalid funding row");
            const Funding f{csv.time(), *rate, *mark};
            require(data.funding.empty() || f.time > data.funding.back().time, "Funding must be sorted");
            data.funding.push_back(f);
        }
    }
    require(data.prices.size() > 60, "Insufficient overlapping Kalshi and spot prices");
    return data;
}

// ---------------------------------------------------------------- backtest

enum class Strategy { Both, Long, Short };
enum class Fill { Mid, Touch };

struct Settings {
    Strategy strategy = Strategy::Both;
    Fill fill = Fill::Mid;
    int holding_minutes = 5;
    double threshold_bps = 5;
    std::optional<double> exit_bps;  // convergence target; empty = threshold, as in the primary run
    double exit_target() const { return exit_bps.value_or(threshold_bps); }
    bool exit_on_profit = true;
    double quantity_btc = 0.01, minimum_edge_bps = 0;
};

struct Trade {
    Time entry = 0, exit = 0;
    int direction = 0;  // +1 = long perp / short spot; -1 = short perp / long spot
    double entry_spot = 0, entry_perp = 0, exit_spot = 0, exit_perp = 0;
    double entry_basis_bps = 0, exit_basis_bps = 0, entry_spread_bps = 0;
    double previous_funding_bps = 0, forecast_funding_bps = 0, forecast_edge_bps = 0;
    double price_pnl = 0, funding_pnl = 0, net_pnl = 0, net_bps = 0;
    int funding_payments = 0;
    bool forced_end = false;
    std::string exit_reason;
};
struct Equity { Time time = 0; double price_pnl = 0, funding_pnl = 0, net_pnl = 0; int direction = 0; };
struct Result {
    std::vector<Trade> trades;
    std::vector<Equity> equity;
    std::size_t missing_minutes = 0, delayed_exits = 0;
    double price_pnl = 0, funding_pnl = 0, net_pnl = 0, max_drawdown = 0, mean_trade_bps = 0;
    int wins = 0, long_trades = 0, short_trades = 0;
};

// Perp price paid to open (direction) or received to close (-direction).
double perp_price(const Minute& m, int side, Fill fill) {
    if (fill == Fill::Mid) return (m.bid + m.ask) / 2;
    return side > 0 ? m.ask : m.bid;
}
double basis_bps(double perp, double spot) { return (perp / spot - 1) * BPS; }

Result run(const Dataset& data, const Settings& s) {
    Result result;
    const Time first = data.prices.begin()->first, last = data.prices.rbegin()->first;
    result.missing_minutes = static_cast<std::size_t>((last - first) / 60 + 1) - data.prices.size();
    std::optional<Trade> position;
    std::size_t funding_index = 0;
    for (const auto& [t, m] : data.prices) {
        // Funding BEFORE exits and entries: payments in (entry, exit] belong to the pair.
        while (funding_index < data.funding.size() && data.funding[funding_index].time <= t) {
            const auto& f = data.funding[funding_index++];
            if (position && f.time > position->entry) {
                position->funding_pnl -= position->direction * s.quantity_btc * f.mark * f.rate;
                ++position->funding_payments;
            }
        }
        bool closed_this_minute = false;
        if (position) {
            auto& tr = *position;
            const Time deadline = tr.entry + s.holding_minutes * 60LL;
            // If the deadline quote is absent, close at the first quote observed after it.
            // Dropping the trade instead would select the sample on future data availability.
            const bool late = t > deadline;
            result.delayed_exits += late;
            const double perp = perp_price(m, -tr.direction, s.fill);
            const double basis = basis_bps(perp, m.spot);
            const double price = tr.direction * s.quantity_btc * ((perp - tr.entry_perp) - (m.spot - tr.entry_spot));
            const bool profit = s.exit_on_profit && price + tr.funding_pnl > 1e-10;
            const bool converged = tr.direction > 0 ? basis >= -s.exit_target() - 1e-10
                                                    : basis <= s.exit_target() + 1e-10;
            if (profit || converged || t >= deadline || t == last) {
                tr.exit = t; tr.exit_spot = m.spot; tr.exit_perp = perp; tr.exit_basis_bps = basis;
                tr.price_pnl = price; tr.net_pnl = price + tr.funding_pnl;
                tr.net_bps = tr.net_pnl / (s.quantity_btc * tr.entry_spot) * BPS;
                tr.forced_end = !profit && !converged && t < deadline;
                tr.exit_reason = late ? "LATE_TIMEOUT" : profit ? "PROFIT" : converged ? "CONVERGED"
                               : t == deadline ? "TIMEOUT" : "DATA_END";
                result.price_pnl += tr.price_pnl; result.funding_pnl += tr.funding_pnl;
                result.mean_trade_bps += tr.net_bps; result.wins += tr.net_pnl > 0;
                result.long_trades += tr.direction > 0; result.short_trades += tr.direction < 0;
                result.trades.push_back(tr);
                position.reset(); closed_this_minute = true;
            }
        }
        if (position || closed_this_minute || t == last) continue;

        int direction = 0;
        if (s.strategy != Strategy::Short && basis_bps(perp_price(m, 1, s.fill), m.spot) < -s.threshold_bps - 1e-10)
            direction = 1;
        else if (s.strategy != Strategy::Long && basis_bps(perp_price(m, -1, s.fill), m.spot) > s.threshold_bps + 1e-10)
            direction = -1;
        if (!direction) continue;

        // Theo edge: basis move to the threshold target, minus funding the side
        // expects to pay if an 8h boundary falls inside the holding window.
        const double perp = perp_price(m, direction, s.fill);
        const double basis = basis_bps(perp, m.spot);
        const double previous = funding_index ? data.funding[funding_index - 1].rate * BPS : 0;
        // Next boundary from the fixed settlement schedule, not from the next row in the file.
        const Time next_funding = t - (t - FUNDING_OFFSET) % FUNDING_PERIOD + FUNDING_PERIOD;
        const bool crosses = next_funding <= t + s.holding_minutes * 60LL;
        const double funding_paid = crosses ? previous : 0;  // paid by a long; received by a short
        const double edge = direction * (-direction * s.exit_target() - basis) - direction * funding_paid;
        if (edge <= s.minimum_edge_bps) continue;
        Trade tr;
        tr.entry = t; tr.direction = direction; tr.entry_spot = m.spot; tr.entry_perp = perp;
        tr.entry_basis_bps = basis; tr.entry_spread_bps = (m.ask - m.bid) / m.spot * BPS;
        tr.previous_funding_bps = previous; tr.forecast_funding_bps = funding_paid; tr.forecast_edge_bps = edge;
        position = tr;
    }
    require(!position, "Final position must close");
    result.net_pnl = result.price_pnl + result.funding_pnl;
    if (!result.trades.empty()) result.mean_trade_bps /= result.trades.size();

    // Equity from closed trades, marked at the price the pair would exit at.
    std::size_t trade_index = 0;
    double closed_price = 0, closed_funding = 0, peak = 0;
    for (const auto& [t, m] : data.prices) {
        while (trade_index < result.trades.size() && result.trades[trade_index].exit < t) {
            closed_price += result.trades[trade_index].price_pnl;
            closed_funding += result.trades[trade_index].funding_pnl;
            ++trade_index;
        }
        double price = closed_price, funding = closed_funding;
        int direction = 0;
        if (trade_index < result.trades.size() && result.trades[trade_index].entry <= t) {
            const auto& tr = result.trades[trade_index];
            const double perp = t == tr.exit ? tr.exit_perp : perp_price(m, -tr.direction, s.fill);
            price += tr.direction * s.quantity_btc * ((perp - tr.entry_perp) - (m.spot - tr.entry_spot));
            for (const auto& f : data.funding)
                if (f.time > tr.entry && f.time <= t) funding -= tr.direction * s.quantity_btc * f.mark * f.rate;
            if (t < tr.exit) direction = tr.direction;
        }
        const double net = price + funding;
        peak = std::max(peak, net);
        result.max_drawdown = std::max(result.max_drawdown, peak - net);
        result.equity.push_back({t, price, funding, net, direction});
    }
    return result;
}

// ---------------------------------------------------------------- output

const char* strategy_name(Strategy s) {
    return s == Strategy::Both ? "both_basis_threshold" : s == Strategy::Long ? "long_basis_threshold"
                                                                              : "short_basis_threshold";
}

std::ofstream output(const std::filesystem::path& path) {
    std::ofstream file(path);
    require(file.good(), "Cannot write " + path.string());
    file << std::fixed << std::setprecision(10);
    return file;
}

void write_results(const Dataset& data, const Settings& s, const Result& r, const std::string& directory) {
    const std::filesystem::path dir(directory);
    std::filesystem::create_directories(dir);
    auto trades = output(dir / "trades.csv");
    trades << "entry_ts,exit_ts,entry_utc,exit_utc,side,quantity_btc,entry_spot,entry_perp,exit_spot,exit_perp,"
              "entry_basis_bps,exit_basis_bps,entry_spread_bps,previous_funding_bps,forecast_funding_bps,"
              "forecast_edge_bps,price_pnl,funding_pnl,net_pnl,net_bps,funding_payments,forced_end,exit_reason\n";
    for (const auto& tr : r.trades)
        trades << tr.entry << ',' << tr.exit << ',' << utc(tr.entry) << ',' << utc(tr.exit) << ','
               << (tr.direction > 0 ? "LONG_PERP_SHORT_SPOT" : "SHORT_PERP_LONG_SPOT") << ',' << s.quantity_btc << ','
               << tr.entry_spot << ',' << tr.entry_perp << ',' << tr.exit_spot << ',' << tr.exit_perp << ','
               << tr.entry_basis_bps << ',' << tr.exit_basis_bps << ',' << tr.entry_spread_bps << ','
               << tr.previous_funding_bps << ',' << tr.forecast_funding_bps << ',' << tr.forecast_edge_bps << ','
               << tr.price_pnl << ',' << tr.funding_pnl << ',' << tr.net_pnl << ',' << tr.net_bps << ','
               << tr.funding_payments << ',' << tr.forced_end << ',' << tr.exit_reason << '\n';
    auto equity = output(dir / "equity.csv");
    equity << "ts,utc,price_pnl,funding_pnl,net_pnl,direction\n";
    for (const auto& e : r.equity)
        equity << e.time << ',' << utc(e.time) << ',' << e.price_pnl << ',' << e.funding_pnl << ','
               << e.net_pnl << ',' << e.direction << '\n';
    auto summary = output(dir / "summary.json");
    summary << "{\n  \"start_utc\": \"" << utc(data.prices.begin()->first) << "\",\n"
            << "  \"end_utc\": \"" << utc(data.prices.rbegin()->first) << "\",\n"
            << "  \"venue\": \"kalshi_KXBTCPERP_vs_coinbase_BTC-USD\",\n"
            << "  \"fill\": \"" << (s.fill == Fill::Mid ? "mid" : "touch") << "\",\n"
            << "  \"holding_minutes\": " << s.holding_minutes << ",\n"
            << "  \"strategy\": \"" << strategy_name(s.strategy) << "\",\n"
            << "  \"discount_threshold_bps\": " << s.threshold_bps << ",\n"
            << "  \"exit_target_bps\": " << s.exit_target() << ",\n"
            << "  \"exit_on_profit\": " << (s.exit_on_profit ? "true" : "false") << ",\n"
            << "  \"quantity_btc\": " << s.quantity_btc << ",\n  \"cost_bps\": 0,\n"
            << "  \"minimum_edge_bps\": " << s.minimum_edge_bps << ",\n"
            << "  \"funding_pnl_source\": \"recorded_8h_rates_and_marks\",\n"
            << "  \"spot_rows\": " << data.spot_rows << ",\n  \"perp_rows\": " << data.perp_rows << ",\n"
            << "  \"bad_perp_quotes\": " << data.bad_quotes << ",\n"
            << "  \"matched_rows\": " << data.prices.size() << ",\n"
            << "  \"missing_matched_minutes\": " << r.missing_minutes << ",\n"
            << "  \"delayed_exits\": " << r.delayed_exits << ",\n"
            << "  \"trades\": " << r.trades.size() << ",\n  \"long_trades\": " << r.long_trades << ",\n"
            << "  \"short_trades\": " << r.short_trades << ",\n  \"winning_trades\": " << r.wins << ",\n"
            << "  \"price_pnl\": " << r.price_pnl << ",\n  \"funding_pnl\": " << r.funding_pnl << ",\n"
            << "  \"net_pnl\": " << r.net_pnl << ",\n  \"max_drawdown\": " << r.max_drawdown << ",\n"
            << "  \"mean_trade_bps\": " << r.mean_trade_bps << "\n}\n";
    require(trades.good() && equity.good() && summary.good(), "Failed to write complete results");
}

Time utc_date(const std::string& text) {
    std::tm tm{};
    std::istringstream in(text);
    in >> std::get_time(&tm, "%Y-%m-%d");
    require(!in.fail(), "Dates must be YYYY-MM-DD: " + text);
    return static_cast<Time>(timegm(&tm));
}

double number(const std::string& text) {
    const auto value = parse(text);
    require(bool(value), "Invalid number: " + text);
    return *value;
}
}  // namespace

int main(int argc, char** argv) {
    try {
        Settings settings;
        // Defaults assume running from Kalshi-Market-Model (including make backtest).
        std::string spot = "../data/coinbase/BTC-USD_spot_1m.csv";
        std::string perp = "../data/kalshi/KXBTCPERP_candles_1m.csv";
        std::string funding = "../data/kalshi/KXBTCPERP_funding.csv";
        std::string results = "results/basis-both-5m";
        Time start_time = 0;
        for (int i = 1; i < argc; ++i) {
            const std::string option = argv[i];
            if (option == "--help") {
                std::cout << "Zero-cost Kalshi basis backtest: buy below -5 bps; sell above +5 bps.\n"
                          << "--spot-csv FILE --perp-csv FILE --funding-csv FILE --output DIRECTORY\n"
                          << "--hold-minutes 5 --threshold-bps 5 --qty-btc 0.01 --min-edge-bps 0\n"
                          << "--exit-bps X (convergence target; default = threshold, e.g. 1 for Kalshi's funding dead zone)\n"
                          << "--profit-exit 1 (0 waits for convergence or timeout)\n"
                          << "--strategy both-basis (default), long-basis, or short-basis\n"
                          << "--fill mid (default; idealized midpoint) or touch (cross the Kalshi spread)\n"
                          << "--start YYYY-MM-DD (UTC; drop earlier minutes, e.g. the thin launch-week book)\n";
                return 0;
            }
            if (++i >= argc) throw std::runtime_error("Missing value for " + option);
            const std::string value = argv[i];
            if (option == "--spot-csv") spot = value;
            else if (option == "--perp-csv") perp = value;
            else if (option == "--funding-csv") funding = value;
            else if (option == "--output") results = value;
            else if (option == "--start") start_time = utc_date(value);
            else if (option == "--qty-btc") settings.quantity_btc = number(value);
            else if (option == "--min-edge-bps") settings.minimum_edge_bps = number(value);
            else if (option == "--threshold-bps") settings.threshold_bps = number(value);
            else if (option == "--exit-bps") settings.exit_bps = number(value);
            else if (option == "--hold-minutes") settings.holding_minutes = static_cast<int>(number(value));
            else if (option == "--profit-exit") settings.exit_on_profit = value == "1";
            else if (option == "--fill") {
                require(value == "mid" || value == "touch", "--fill must be mid or touch");
                settings.fill = value == "mid" ? Fill::Mid : Fill::Touch;
            } else if (option == "--strategy") {
                if (value == "both-basis") settings.strategy = Strategy::Both;
                else if (value == "long-basis") settings.strategy = Strategy::Long;
                else if (value == "short-basis") settings.strategy = Strategy::Short;
                else throw std::runtime_error("Unknown strategy");
            } else throw std::runtime_error("Unknown option: " + option);
        }
        require(settings.holding_minutes > 0 && settings.quantity_btc > 0 && settings.threshold_bps >= 0 && settings.exit_target() >= 0 &&
                settings.minimum_edge_bps >= 0, "Invalid settings");
        const auto data = load(spot, perp, funding, start_time);
        const auto run_result = run(data, settings);
        write_results(data, settings, run_result, results);
        const auto& r = run_result;
        std::cout << std::fixed << std::setprecision(6)
                  << "KALSHI ZERO-COST BASIS BACKTEST (" << (settings.fill == Fill::Mid ? "mid" : "touch") << " fills)\n"
                  << "UTC period: " << utc(data.prices.begin()->first) << " through " << utc(data.prices.rbegin()->first) << '\n'
                  << "Strategy: " << strategy_name(settings.strategy) << "; entry " << settings.threshold_bps << " bps, exit " << settings.exit_target()
                  << " bps; max hold " << settings.holding_minutes << " min; " << settings.quantity_btc << " BTC per leg\n"
                  << "Trades: " << r.trades.size() << " (long basis " << r.long_trades << ", short basis " << r.short_trades << ")\n"
                  << "BASIS PRICE PnL: " << r.price_pnl << '\n'
                  << "REALIZED FUNDING PnL: " << r.funding_pnl << '\n'
                  << "TOTAL PnL: " << r.net_pnl << '\n'
                  << "Mean trade PnL: " << r.mean_trade_bps << " bps of entry spot notional\n"
                  << "Win rate: " << (r.trades.empty() ? 0 : 100.0 * r.wins / r.trades.size()) << "%\n"
                  << "Maximum mark-to-market drawdown: " << r.max_drawdown << '\n'
                  << "Matched minutes: " << data.prices.size() << "; missing: " << r.missing_minutes
                  << "; bad Kalshi quotes dropped: " << data.bad_quotes
                  << "; late exits (deadline quote missing): " << r.delayed_exits << '\n'
                  << "Results: " << results << "/{summary.json,trades.csv,equity.csv}\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << '\n';
        return 1;
    }
}
