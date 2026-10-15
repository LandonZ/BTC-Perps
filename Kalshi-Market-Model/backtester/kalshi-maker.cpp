// Maker simulation of the Kalshi basis trade on tick data.
//
// kalshi-backtester.cpp crosses the Kalshi spread when the perp is rich or cheap
// to spot. This instead rests post-only perp quotes around a fair value taken
// from Coinbase spot, and hedges each fill with a spot market order:
//   fair = spot * (1 + center)   center: fixed bps, or a trailing EMA of the basis
//   sell perp at fair * (1 + edge), buy perp at fair * (1 - edge)
//   once filled, buy/sell the same BTC of spot after --hedge-latency seconds,
//   then rest a maker order to close at fair +/- exit, or cross the Kalshi
//   spread at the holding deadline.
//
// Fill model (the main assumption): the public Kalshi tape has trades but no
// order book. A resting order fills when takers trade at its price
// (`--queue front`) or through it (`--queue back`: the whole level traded, so
// our place in line does not matter). Fills accumulate until the full size is
// done. The best bid/ask are the last seller-/buyer-initiated prints
// (taker_side "ask" = seller hit the bid, "bid" = buyer lifted the ask); quotes
// never cross them (post-only).
#include <algorithm>
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
constexpr double CONTRACT_BTC = 0.0001;  // Kalshi quotes per 0.0001 BTC; one tick = $1 per BTC

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
    double number(const std::string& name) const {
        const auto value = parse(cell(name));
        require(bool(value), "Invalid " + name + ": " + cell(name));
        return *value;
    }
};

std::string utc(double value) {
    const std::time_t t = static_cast<std::time_t>(std::floor(value));
    std::tm tm{};
    require(gmtime_r(&t, &tm) != nullptr, "Invalid UTC timestamp");
    char text[32];
    std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return text;
}

// Every file in `directory` whose name starts with `prefix`, in name (= date) order.
std::vector<std::string> daily_files(const std::string& directory, const std::string& prefix) {
    std::vector<std::string> paths;
    require(std::filesystem::is_directory(directory), "Missing directory " + directory);
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        const auto name = entry.path().filename().string();
        if (name.rfind(prefix, 0) == 0 && entry.path().extension() == ".csv") paths.push_back(entry.path().string());
    }
    std::sort(paths.begin(), paths.end());
    require(!paths.empty(), "No " + prefix + "*.csv files in " + directory);
    return paths;
}

// ---------------------------------------------------------------- data

struct Print { double time = 0; std::int64_t price = 0; double size = 0; bool taker_buy = false; };  // price: USD per BTC
struct SpotSecond { Time time = 0; double close = 0; };  // last spot trade of the second starting at `time`
struct Funding { Time time = 0; double rate = 0, mark = 0; };
struct Dataset {
    std::vector<Print> prints;
    std::vector<SpotSecond> spot;
    std::vector<Funding> funding;
};

Dataset load(const std::string& trades_dir, const std::string& spot_dir, const std::string& funding_file,
             double start, double end) {
    Dataset data;
    for (const auto& path : daily_files(trades_dir, "KXBTCPERP_trades_")) {
        Csv csv(path);
        while (csv.next()) {
            const double t = csv.number("ts");
            if (t < start || t >= end) continue;
            const std::string& side = csv.cell("taker_side");
            require(side == "bid" || side == "ask", "Unknown taker_side " + side);
            const auto price = std::llround(csv.number("price") / CONTRACT_BTC);
            const double size = csv.number("count") * CONTRACT_BTC;
            require(price > 0 && size > 0, "Invalid Kalshi trade in " + path);
            data.prints.push_back({t, price, size, side == "bid"});
        }
    }
    std::stable_sort(data.prints.begin(), data.prints.end(),
                     [](const Print& a, const Print& b) { return a.time < b.time; });
    for (const auto& path : daily_files(spot_dir, "BTC-USD_spot_1s_")) {
        Csv csv(path);
        while (csv.next()) {
            const SpotSecond s{static_cast<Time>(csv.number("ts")), csv.number("close")};
            require(s.close > 0, "Invalid spot price in " + path);
            require(data.spot.empty() || s.time > data.spot.back().time, "Spot seconds must be sorted");
            data.spot.push_back(s);
        }
    }
    Csv csv(funding_file);
    while (csv.next())
        data.funding.push_back({static_cast<Time>(csv.number("ts")), csv.number("funding_rate"),
                                csv.number("mark_price_usd_per_btc")});
    require(data.prints.size() > 1000, "Too few Kalshi trades in the selected period");
    return data;
}

// Last spot price known at time t: the close of the latest second that ended by t.
struct SpotQuote { double price = 0, age = 0; };
std::optional<SpotQuote> spot_at(const std::vector<SpotSecond>& spot, double t) {
    const auto it = std::upper_bound(spot.begin(), spot.end(), t - 1,
                                     [](double v, const SpotSecond& s) { return v < static_cast<double>(s.time); });
    if (it == spot.begin()) return {};
    const auto& s = *std::prev(it);
    return SpotQuote{s.close, t - static_cast<double>(s.time + 1)};
}

// ---------------------------------------------------------------- simulation

enum class Queue { Front, Back };

struct Settings {
    double edge_bps = 2, exit_bps = 0;
    bool trailing_center = false;
    double center_bps = 0, center_minutes = 60;
    double holding_minutes = 5;
    Queue queue = Queue::Back;
    double quantity_btc = 0.01;
    double quote_latency = 1, hedge_latency = 1, max_spot_age = 5;
    bool quote_buy = true, quote_sell = true;
    double perp_maker_fee_bps = 0, perp_taker_fee_bps = 0, spot_fee_bps = 0;
};

struct Trade {
    double entry = 0, exit = 0;
    int direction = 0;  // +1 = long perp / short spot; -1 = short perp / long spot
    double entry_perp = 0, quote_spot = 0, entry_spot = 0, exit_perp = 0, exit_spot = 0, center_bps = 0;
    double entry_basis_bps = 0, hedge_slip_bps = 0, exit_basis_bps = 0;
    double price_pnl = 0, fee_pnl = 0, funding_pnl = 0, net_pnl = 0, net_bps = 0;
    std::string exit_reason;
};
struct Equity { Time time = 0; double net_pnl = 0; int direction = 0; };
struct Result {
    std::vector<Trade> trades;
    std::vector<Equity> equity;
    double price_pnl = 0, fee_pnl = 0, funding_pnl = 0, net_pnl = 0, max_drawdown = 0;
    double mean_entry_basis_bps = 0, mean_hedge_slip_bps = 0, mean_trade_bps = 0;
    int wins = 0, long_trades = 0, short_trades = 0, timeouts = 0;
    std::size_t stale_spot_prints = 0;
};

// A resting order collecting fills from qualifying prints.
struct Order {
    double filled = 0, notional = 0;
    void add(double size, std::int64_t price) { filled += size; notional += size * static_cast<double>(price); }
    double average() const { return notional / filled; }
};

Result run(const Dataset& data, const Settings& s) {
    Result result;
    std::optional<Trade> position;
    Order buy, sell, close;
    std::int64_t best_bid = 0, best_ask = 0;  // 0 = unknown
    double ema = 0, ema_time = 0, ema_start = -1, peak = 0, last_spot = 0;
    std::int64_t last_print = 0;
    Time minute = 0;
    std::size_t funding_index = 0;
    const double tau = s.center_minutes * 60;
    const auto fee = [&](double bps, double price) { return -bps / BPS * s.quantity_btc * price; };

    const auto finish = [&](double t, double perp, double spot, double perp_fee_bps, const std::string& reason) {
        auto& tr = *position;
        tr.exit = t; tr.exit_perp = perp; tr.exit_spot = spot;
        tr.exit_basis_bps = (perp / spot - 1) * BPS; tr.exit_reason = reason;
        tr.price_pnl = tr.direction * s.quantity_btc * ((perp - tr.entry_perp) - (spot - tr.entry_spot));
        tr.fee_pnl += fee(perp_fee_bps, perp) + fee(s.spot_fee_bps, spot);
        tr.net_pnl = tr.price_pnl + tr.fee_pnl + tr.funding_pnl;
        tr.net_bps = tr.net_pnl / (s.quantity_btc * tr.entry_spot) * BPS;
        result.price_pnl += tr.price_pnl; result.fee_pnl += tr.fee_pnl; result.funding_pnl += tr.funding_pnl;
        result.wins += tr.net_pnl > 0; result.timeouts += reason != "TARGET";
        result.long_trades += tr.direction > 0; result.short_trades += tr.direction < 0;
        result.mean_entry_basis_bps += -tr.direction * tr.entry_basis_bps;  // captured: + = sold rich / bought cheap
        result.mean_hedge_slip_bps += tr.hedge_slip_bps;
        result.mean_trade_bps += tr.net_bps;
        result.trades.push_back(tr);
        position.reset(); close = {};
    };

    for (std::size_t i = 0; i < data.prints.size(); ++i) {
        const auto& p = data.prints[i];
        const double t = p.time;

        // Minute equity, marked at the last print and spot before this one.
        if (static_cast<Time>(t) / 60 * 60 != minute) {
            if (minute && last_spot > 0) {
                double net = result.price_pnl + result.fee_pnl + result.funding_pnl;
                if (position) {
                    const auto& tr = *position;
                    net += tr.direction * s.quantity_btc * ((last_print - tr.entry_perp) - (last_spot - tr.entry_spot))
                         + tr.fee_pnl + tr.funding_pnl;
                }
                peak = std::max(peak, net);
                result.max_drawdown = std::max(result.max_drawdown, peak - net);
                result.equity.push_back({minute, net, position ? position->direction : 0});
            }
            minute = static_cast<Time>(t) / 60 * 60;
        }
        while (funding_index < data.funding.size() && data.funding[funding_index].time <= t) {
            const auto& f = data.funding[funding_index++];
            if (position && f.time > position->entry)
                position->funding_pnl -= position->direction * s.quantity_btc * f.mark * f.rate;
        }

        const auto spot = spot_at(data.spot, t - s.quote_latency);
        const auto fill_spot = spot_at(data.spot, t + s.hedge_latency);  // where a spot market order executes
        const bool usable = spot && fill_spot && spot->age <= s.max_spot_age;
        if (!usable) ++result.stale_spot_prints;
        if (usable) {
            const double basis = (static_cast<double>(p.price) / spot->price - 1) * BPS;
            const bool warm = !s.trailing_center || (ema_start >= 0 && t - ema_start >= tau);
            const double center = s.trailing_center ? ema : s.center_bps;
            const double fair = spot->price * (1 + center / BPS);
            // A taker buy fills resting asks priced at or below it; a taker sell fills bids at or above.
            const auto lifts = [&](std::int64_t ask) {
                return p.taker_buy && (p.price > ask || (s.queue == Queue::Front && p.price == ask));
            };
            const auto hits = [&](std::int64_t bid) {
                return !p.taker_buy && (p.price < bid || (s.queue == Queue::Front && p.price == bid));
            };
            const auto ask_at = [&](double bps) {  // post-only: never at or below the best bid
                const auto a = static_cast<std::int64_t>(std::ceil(fair * (1 + bps / BPS)));
                return best_bid ? std::max(a, best_bid + 1) : a;
            };
            const auto bid_at = [&](double bps) {
                const auto b = static_cast<std::int64_t>(std::floor(fair * (1 - bps / BPS)));
                return best_ask ? std::min(b, best_ask - 1) : b;
            };

            if (position) {
                auto& tr = *position;
                if (t >= tr.entry + s.holding_minutes * 60) {
                    // Deadline: cross the Kalshi spread and unwind spot at market.
                    const std::int64_t touch = tr.direction > 0 ? best_bid : best_ask;
                    if (touch) finish(t, static_cast<double>(touch), fill_spot->price, s.perp_taker_fee_bps, "TIMEOUT");
                } else {
                    const std::int64_t target = tr.direction > 0 ? ask_at(-s.exit_bps) : bid_at(-s.exit_bps);
                    if (tr.direction > 0 ? lifts(target) : hits(target)) {
                        close.add(std::min(p.size, s.quantity_btc - close.filled), target);
                        if (close.filled >= s.quantity_btc - 1e-12)
                            finish(t, close.average(), fill_spot->price, s.perp_maker_fee_bps, "TARGET");
                    }
                }
            } else if (warm) {
                const std::int64_t ask = ask_at(s.edge_bps), bid = bid_at(s.edge_bps);
                Order* order = nullptr;
                int direction = 0;
                if (s.quote_sell && lifts(ask)) { sell.add(std::min(p.size, s.quantity_btc - sell.filled), ask); order = &sell; direction = -1; }
                if (s.quote_buy && hits(bid)) { buy.add(std::min(p.size, s.quantity_btc - buy.filled), bid); order = &buy; direction = 1; }
                if (order && order->filled >= s.quantity_btc - 1e-12) {
                    Trade tr;
                    tr.entry = t; tr.direction = direction; tr.entry_perp = order->average();
                    tr.quote_spot = spot->price; tr.entry_spot = fill_spot->price; tr.center_bps = center;
                    tr.entry_basis_bps = (tr.entry_perp / spot->price - 1) * BPS;
                    // Positive = the hedge cost money: spot moved against us between quote and hedge.
                    tr.hedge_slip_bps = -direction * (fill_spot->price / spot->price - 1) * BPS;
                    tr.fee_pnl = fee(s.perp_maker_fee_bps, tr.entry_perp) + fee(s.spot_fee_bps, tr.entry_spot);
                    position = tr; buy = {}; sell = {};
                }
            }
            // Trailing centre: time-weighted EMA of the print-vs-spot basis.
            if (ema_start < 0) { ema_start = ema_time = t; ema = basis; }
            ema += (1 - std::exp(-(t - ema_time) / tau)) * (basis - ema);
            ema_time = t;
            last_spot = spot->price;
        }
        // Update the estimated touch after quoting: this print only informs later decisions.
        if (p.taker_buy) { best_ask = p.price; if (best_bid >= p.price) best_bid = p.price - 1; }
        else { best_bid = p.price; if (best_ask && best_ask <= p.price) best_ask = p.price + 1; }
        last_print = p.price;
    }
    if (position) {
        const auto spot = spot_at(data.spot, data.prints.back().time + s.hedge_latency);
        require(bool(spot), "No spot price to close the final position");
        const std::int64_t touch = position->direction > 0 ? best_bid : best_ask;
        finish(data.prints.back().time, static_cast<double>(touch), spot->price, s.perp_taker_fee_bps, "DATA_END");
    }
    result.net_pnl = result.price_pnl + result.fee_pnl + result.funding_pnl;
    if (!result.trades.empty()) {
        const double n = static_cast<double>(result.trades.size());
        result.mean_entry_basis_bps /= n; result.mean_hedge_slip_bps /= n; result.mean_trade_bps /= n;
    }
    return result;
}

// ---------------------------------------------------------------- output

std::ofstream output(const std::filesystem::path& path) {
    std::ofstream file(path);
    require(file.good(), "Cannot write " + path.string());
    file << std::fixed << std::setprecision(6);
    return file;
}

void write_results(const Dataset& data, const Settings& s, const Result& r, const std::string& directory) {
    const std::filesystem::path dir(directory);
    std::filesystem::create_directories(dir);
    auto trades = output(dir / "trades.csv");
    trades << "entry_ts,exit_ts,entry_utc,exit_utc,side,quantity_btc,entry_perp,quote_spot,entry_spot,exit_perp,exit_spot,"
              "center_bps,entry_basis_bps,hedge_slip_bps,exit_basis_bps,price_pnl,fee_pnl,funding_pnl,net_pnl,net_bps,exit_reason\n";
    for (const auto& tr : r.trades)
        trades << tr.entry << ',' << tr.exit << ',' << utc(tr.entry) << ',' << utc(tr.exit) << ','
               << (tr.direction > 0 ? "LONG_PERP_SHORT_SPOT" : "SHORT_PERP_LONG_SPOT") << ',' << s.quantity_btc << ','
               << tr.entry_perp << ',' << tr.quote_spot << ',' << tr.entry_spot << ',' << tr.exit_perp << ',' << tr.exit_spot << ','
               << tr.center_bps << ',' << tr.entry_basis_bps << ',' << tr.hedge_slip_bps << ',' << tr.exit_basis_bps << ','
               << tr.price_pnl << ',' << tr.fee_pnl << ',' << tr.funding_pnl << ',' << tr.net_pnl << ',' << tr.net_bps << ','
               << tr.exit_reason << '\n';
    auto equity = output(dir / "equity.csv");
    equity << "ts,utc,net_pnl,direction\n";
    for (const auto& e : r.equity) equity << e.time << ',' << utc(e.time) << ',' << e.net_pnl << ',' << e.direction << '\n';
    auto summary = output(dir / "summary.json");
    summary << "{\n  \"start_utc\": \"" << utc(data.prints.front().time) << "\",\n"
            << "  \"end_utc\": \"" << utc(data.prints.back().time) << "\",\n"
            << "  \"venue\": \"kalshi_KXBTCPERP_maker_vs_coinbase_BTC-USD\",\n"
            << "  \"edge_bps\": " << s.edge_bps << ",\n  \"exit_bps\": " << s.exit_bps << ",\n"
            << "  \"center\": \"" << (s.trailing_center ? "trailing_ema" : "fixed") << "\",\n"
            << "  \"center_bps\": " << s.center_bps << ",\n  \"center_minutes\": " << s.center_minutes << ",\n"
            << "  \"holding_minutes\": " << s.holding_minutes << ",\n"
            << "  \"queue\": \"" << (s.queue == Queue::Front ? "front" : "back") << "\",\n"
            << "  \"sides\": \"" << (s.quote_buy && s.quote_sell ? "both" : s.quote_sell ? "sell" : "buy") << "\",\n"
            << "  \"quantity_btc\": " << s.quantity_btc << ",\n"
            << "  \"quote_latency_s\": " << s.quote_latency << ",\n  \"hedge_latency_s\": " << s.hedge_latency << ",\n"
            << "  \"perp_maker_fee_bps\": " << s.perp_maker_fee_bps << ",\n"
            << "  \"perp_taker_fee_bps\": " << s.perp_taker_fee_bps << ",\n"
            << "  \"spot_fee_bps\": " << s.spot_fee_bps << ",\n"
            << "  \"kalshi_prints\": " << data.prints.size() << ",\n  \"spot_seconds\": " << data.spot.size() << ",\n"
            << "  \"stale_spot_prints\": " << r.stale_spot_prints << ",\n"
            << "  \"trades\": " << r.trades.size() << ",\n  \"long_trades\": " << r.long_trades << ",\n"
            << "  \"short_trades\": " << r.short_trades << ",\n  \"winning_trades\": " << r.wins << ",\n"
            << "  \"timeouts\": " << r.timeouts << ",\n"
            << "  \"price_pnl\": " << r.price_pnl << ",\n  \"fee_pnl\": " << r.fee_pnl << ",\n"
            << "  \"funding_pnl\": " << r.funding_pnl << ",\n  \"net_pnl\": " << r.net_pnl << ",\n"
            << "  \"max_drawdown\": " << r.max_drawdown << ",\n"
            << "  \"mean_entry_basis_bps\": " << r.mean_entry_basis_bps << ",\n"
            << "  \"mean_hedge_slip_bps\": " << r.mean_hedge_slip_bps << ",\n"
            << "  \"mean_trade_bps\": " << r.mean_trade_bps << "\n}\n";
    require(trades.good() && equity.good() && summary.good(), "Failed to write complete results");
}

double utc_date(const std::string& text) {
    std::tm tm{};
    std::istringstream in(text);
    in >> std::get_time(&tm, "%Y-%m-%d");
    require(!in.fail(), "Dates must be YYYY-MM-DD: " + text);
    return static_cast<double>(timegm(&tm));
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
        // Defaults assume running from Kalshi-Market-Model (including make maker).
        std::string trades = "../data/kalshi/trades", spot = "../data/coinbase/trades";
        std::string funding = "../data/kalshi/KXBTCPERP_funding.csv", results = "results/maker";
        double start = 0, end = 1e12;
        for (int i = 1; i < argc; ++i) {
            const std::string option = argv[i];
            if (option == "--help") {
                std::cout << "Kalshi maker simulation: post-only perp quotes around spot, hedged on Coinbase spot.\n"
                          << "--trades-dir DIR --spot-dir DIR --funding-csv FILE --output DIRECTORY\n"
                          << "--edge-bps 2 (quote distance from fair) --exit-bps 0 (close distance from fair)\n"
                          << "--center-bps 0 (fair = spot + this) or --center-minutes N (fair = spot + trailing N-min EMA basis)\n"
                          << "--hold-minutes 5 (then cross the spread) --qty-btc 0.01 --sides both|sell|buy\n"
                          << "--queue back (fill only when trades go through our price; default) or front (at our price)\n"
                          << "--quote-latency 1 --hedge-latency 1 --max-spot-age 5 (seconds)\n"
                          << "--perp-maker-fee-bps 0 --perp-taker-fee-bps 0 --spot-fee-bps 0 (per side)\n"
                          << "--start YYYY-MM-DD --end YYYY-MM-DD (UTC, end exclusive)\n";
                return 0;
            }
            if (++i >= argc) throw std::runtime_error("Missing value for " + option);
            const std::string value = argv[i];
            if (option == "--trades-dir") trades = value;
            else if (option == "--spot-dir") spot = value;
            else if (option == "--funding-csv") funding = value;
            else if (option == "--output") results = value;
            else if (option == "--start") start = utc_date(value);
            else if (option == "--end") end = utc_date(value);
            else if (option == "--edge-bps") settings.edge_bps = number(value);
            else if (option == "--exit-bps") settings.exit_bps = number(value);
            else if (option == "--center-bps") settings.center_bps = number(value);
            else if (option == "--center-minutes") { settings.trailing_center = true; settings.center_minutes = number(value); }
            else if (option == "--hold-minutes") settings.holding_minutes = number(value);
            else if (option == "--qty-btc") settings.quantity_btc = number(value);
            else if (option == "--quote-latency") settings.quote_latency = number(value);
            else if (option == "--hedge-latency") settings.hedge_latency = number(value);
            else if (option == "--max-spot-age") settings.max_spot_age = number(value);
            else if (option == "--perp-maker-fee-bps") settings.perp_maker_fee_bps = number(value);
            else if (option == "--perp-taker-fee-bps") settings.perp_taker_fee_bps = number(value);
            else if (option == "--spot-fee-bps") settings.spot_fee_bps = number(value);
            else if (option == "--queue") {
                require(value == "front" || value == "back", "--queue must be front or back");
                settings.queue = value == "front" ? Queue::Front : Queue::Back;
            } else if (option == "--sides") {
                require(value == "both" || value == "sell" || value == "buy", "--sides must be both, sell or buy");
                settings.quote_buy = value != "sell"; settings.quote_sell = value != "buy";
            } else throw std::runtime_error("Unknown option: " + option);
        }
        require(settings.holding_minutes > 0 && settings.quantity_btc > 0 && settings.center_minutes > 0 &&
                settings.quote_latency >= 0 && settings.hedge_latency >= 0, "Invalid settings");
        const auto data = load(trades, spot, funding, start, end);
        const auto r = run(data, settings);
        write_results(data, settings, r, results);
        std::cout << std::fixed << std::setprecision(4)
                  << "KALSHI MAKER SIMULATION (" << (settings.queue == Queue::Front ? "front" : "back") << " of queue)\n"
                  << "UTC period: " << utc(data.prints.front().time) << " through " << utc(data.prints.back().time) << '\n'
                  << "Quotes: fair " << (settings.trailing_center ? "spot + trailing basis" : "spot + fixed centre")
                  << " +/- " << settings.edge_bps << " bps; close at fair +/- " << settings.exit_bps << " bps; max hold "
                  << settings.holding_minutes << " min\n"
                  << "Trades: " << r.trades.size() << " (long perp " << r.long_trades << ", short perp " << r.short_trades
                  << "; timeouts " << r.timeouts << ")\n"
                  << "PRICE PnL: " << r.price_pnl << "  FEES: " << r.fee_pnl << "  FUNDING: " << r.funding_pnl << '\n'
                  << "TOTAL PnL: " << r.net_pnl << "  (max drawdown " << r.max_drawdown << ")\n"
                  << "Mean basis captured at fill: " << r.mean_entry_basis_bps << " bps; mean hedge slippage: "
                  << r.mean_hedge_slip_bps << " bps; mean trade: " << r.mean_trade_bps << " bps\n"
                  << "Kalshi prints skipped for stale/missing spot: " << r.stale_spot_prints << " of " << data.prints.size() << '\n'
                  << "Results: " << results << "/{summary.json,trades.csv,equity.csv}\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << '\n';
        return 1;
    }
}
