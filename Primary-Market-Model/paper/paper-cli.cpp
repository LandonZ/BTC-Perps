#include "paper-engine.h"
#include "../backtester/basis-backtest.h"
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace btc::paper;
double number(const std::string& text) {
    std::size_t used = 0;
    const double value = std::stod(text, &used);
    if (used != text.size() || !std::isfinite(value)) throw std::runtime_error("Invalid number " + text);
    return value;
}
bool flag(const std::string& text) {
    if (text != "0" && text != "1") throw std::runtime_error("Expected 0 or 1, got " + text);
    return text == "1";
}
const char* help =
    "Paper trader: BIP perp-style future vs BTC-USD spot, one engine for replay and live.\n"
    "Input (pick one):\n"
    "  --events FILE [--events FILE ...]  recorder events.csv files, in time order ('-' = stdin)\n"
    "  --live                             read events from stdin as they arrive, e.g.\n"
    "      python3 tools/record_coinbase.py --stdout | ./build/paper-trader --live\n"
    "  --candles                          Allen's minute candles as quotes (bid = ask = open);\n"
    "      --spot-csv F --perp-csv F --funding-csv F (defaults match basis-backtester)\n"
    "Strategy:\n"
    "  --mode taker|maker      taker: cross both legs when basis leaves the band (default)\n"
    "                          maker: Allen's theo::price() quotes on the perp, no hedge (cop and lean later)\n"
    "  --latency-ms 0,5,50,250 one row per latency (0 = idealized, then best/middle/worst; candles: 0)\n"
    "  --threshold-bps 5       taker entry band\n"
    "  --ema-halflife-min 10   fair basis = causal EMA of the perp-spot basis (0 = fixed --center-bps;\n"
    "                          candles default to 0 to match Allen's backtest)\n"
    "  --center-bps 0          fixed fair basis when the EMA is off\n"
    "  --contracts 1           0.01 BTC each   --hold-minutes 5   --profit-exit 1\n"
    "  maker: --profit-bps 1 (half-width = (2 maker fees + profit)/2) --skew-bps-per-btc 100\n"
    "         --horizon-min 5 --max-inventory 5 (contracts) --fill-at-touch 0\n"
    "Costs (default 0 -- set your fee tier!):\n"
    "  --perp-taker-bps --perp-maker-bps --perp-fee-usd (per contract) --spot-taker-bps\n"
    "Train/test: --split 0.5 runs the first half (TRAIN) and the rest (TEST) separately\n"
    "Other: --prev-funding-bps 0 --from-ms T --to-ms T --output DIR\n"
    "  --max-gap-ms 10000      feed silence treated as a disconnect: quotes pulled (0 = off)\n"
    "  --perp-product BIP-20DEC30-CDE --spot-product BTC-USD\n";

std::vector<btc::paper::Millis> parse_latencies(const std::string& text) {
    std::vector<Millis> out;
    std::stringstream in(text);
    for (std::string part; std::getline(in, part, ',');) out.push_back(static_cast<Millis>(number(part)));
    if (out.empty()) throw std::runtime_error("No latencies");
    return out;
}

void report(const std::vector<std::unique_ptr<Engine>>& engines) {
    if (engines.empty()) return;
    std::cout << std::fixed << std::setprecision(4);
    if (engines.front()->config().mode == Mode::Maker) {
        std::cout << "latency_ms   fills   buys  sells     net_usd    fees_usd  funding_usd  bps_per_fill    max_dd  position  gaps\n";
        for (const auto& e : engines) {
            int buys = 0; double notional = 0;
            for (const auto& f : e->fills()) { buys += f.side > 0; notional += f.btc * f.price; }
            const auto n = e->fills().size();
            const auto total = e->totals();
            std::cout << std::setw(10) << e->config().latency_ms << std::setw(8) << n << std::setw(7) << buys
                      << std::setw(7) << n - buys << std::setw(12) << total.net() << std::setw(12) << total.fees
                      << std::setw(13) << total.funding_pnl << std::setw(14) << (notional > 0 ? total.net() / notional * 1e4 : 0)
                      << std::setw(10) << e->max_drawdown() << std::setw(10) << e->position_contracts()
                      << std::setw(6) << e->gaps() << '\n';
        }
        std::cout << "(net includes open inventory marked at the perp mid; bps_per_fill = net / filled notional)\n";
        return;
    }
    std::cout << "latency_ms  trades   win%     net_usd    fees_usd  funding_usd  mean_bps  entry_slip_bps    max_dd  position  gaps\n";
    for (const auto& e : engines) {
        const auto& trips = e->trips();
        int wins = 0; double bps = 0, slip = 0;
        for (const auto& t : trips) {
            wins += t.net_pnl > 0; bps += t.net_bps;
            slip += t.direction > 0 ? t.entry_basis_bps - t.signal_basis_bps : t.signal_basis_bps - t.entry_basis_bps;
        }
        const auto n = trips.size();
        const auto total = e->totals();
        std::cout << std::setw(10) << e->config().latency_ms << std::setw(8) << n
                  << std::setw(7) << std::setprecision(1) << (n ? 100.0 * wins / n : 0) << std::setprecision(4)
                  << std::setw(12) << total.net() << std::setw(12) << total.fees << std::setw(13) << total.funding_pnl
                  << std::setw(10) << (n ? bps / n : 0) << std::setw(16) << (n ? slip / n : 0)
                  << std::setw(10) << e->max_drawdown() << std::setw(10) << e->position_contracts()
                  << std::setw(6) << e->gaps() << '\n';
    }
}
} // namespace

int main(int argc, char** argv) {
    try {
        Config base;
        std::vector<std::string> event_files;
        bool live = false, candles = false;
        std::string spot_csv = "../data/coinbase/BTC-USD_spot_1m.csv";
        std::string perp_csv = "../data/coinbase/BTC-PERP-INTX_1m.csv";
        std::string funding_csv = "../data/coinbase/BTC-PERP-INTX_funding_1h.csv";
        std::string output = "results/paper", perp_product = "BIP-20DEC30-CDE", spot_product = "BTC-USD";
        std::string latencies;
        Millis from = 0, to = INT64_MAX;
        bool ema_given = false;
        double split = 0;
        for (int i = 1; i < argc; ++i) {
            const std::string option = argv[i];
            if (option == "--help") { std::cout << help; return 0; }
            if (option == "--live") { live = true; continue; }
            if (option == "--candles") { candles = true; continue; }
            if (++i >= argc) throw std::runtime_error("Missing value for " + option);
            const std::string value = argv[i];
            if (option == "--events") event_files.push_back(value);
            else if (option == "--spot-csv") spot_csv = value;
            else if (option == "--perp-csv") perp_csv = value;
            else if (option == "--funding-csv") funding_csv = value;
            else if (option == "--output") output = value;
            else if (option == "--perp-product") perp_product = value;
            else if (option == "--spot-product") spot_product = value;
            else if (option == "--mode") {
                if (value == "taker") base.mode = Mode::Taker;
                else if (value == "maker") base.mode = Mode::Maker;
                else throw std::runtime_error("--mode must be taker or maker");
            }
            else if (option == "--latency-ms") latencies = value;
            else if (option == "--threshold-bps") base.threshold_bps = number(value);
            else if (option == "--center-bps") base.center_bps = number(value);
            else if (option == "--ema-halflife-min") { base.ema_halflife_min = number(value); ema_given = true; }
            else if (option == "--max-gap-ms") base.max_gap_ms = static_cast<Millis>(number(value));
            else if (option == "--split") {
                split = number(value);
                if (split <= 0 || split >= 1) throw std::runtime_error("--split must be between 0 and 1");
            }
            else if (option == "--contracts") base.contracts = static_cast<int>(number(value));
            else if (option == "--hold-minutes") base.max_hold_ms = static_cast<Millis>(number(value) * 60000);
            else if (option == "--profit-exit") base.exit_on_profit = flag(value);
            else if (option == "--max-inventory") base.max_inventory_contracts = static_cast<int>(number(value));
            else if (option == "--profit-bps") base.profit_bps = number(value);
            else if (option == "--skew-bps-per-btc") base.skew_bps_per_btc = number(value);
            else if (option == "--horizon-min") base.horizon_min = number(value);
            else if (option == "--fill-at-touch") base.fill_at_touch = flag(value);
            else if (option == "--perp-taker-bps") base.perp_taker_bps = number(value);
            else if (option == "--perp-maker-bps") base.perp_maker_bps = number(value);
            else if (option == "--perp-fee-usd") base.perp_fee_usd_per_contract = number(value);
            else if (option == "--spot-taker-bps") base.spot_taker_bps = number(value);
            else if (option == "--prev-funding-bps") base.previous_funding_bps = number(value);
            else if (option == "--from-ms") from = static_cast<Millis>(number(value));
            else if (option == "--to-ms") to = static_cast<Millis>(number(value));
            else throw std::runtime_error("Unknown option " + option + " (see --help)");
        }
        if (int(live) + int(candles) + int(!event_files.empty()) != 1)
            throw std::runtime_error("Choose exactly one of --events, --live or --candles (see --help)");

        if (split && event_files.empty()) throw std::runtime_error("--split needs --events files");
        if (candles) {
            // Candle quotes change once a minute: a latency would just mean "fill at the next open",
            // minute spacing is not a disconnect, and Allen's backtest uses a fixed center.
            base.max_gap_ms = 0;
            if (!ema_given) base.ema_halflife_min = 0;
        }
        if (latencies.empty()) latencies = candles ? "0" : "0,5,50,250";

        struct Run { std::string name, directory; Millis from, to; std::vector<std::unique_ptr<Engine>> engines; };
        std::vector<Run> runs;
        if (split) {
            // Pre-scan for the time span so the split point uses no information from inside the windows.
            Millis first = INT64_MAX, last = INT64_MIN;
            for (const auto& name : event_files) {
                std::ifstream file(name);
                if (!file) throw std::runtime_error("Cannot read " + name);
                EventReader reader(file, perp_product, spot_product);
                for (Event e; reader.next(e);)
                    if (e.time >= from && e.time < to) { first = std::min(first, e.time); last = std::max(last, e.time); }
            }
            if (first > last) throw std::runtime_error("No events to split");
            const Millis cut = first + static_cast<Millis>((last - first) * split);
            runs.push_back({"TRAIN", output + "/train", from, cut, {}});
            runs.push_back({"TEST", output + "/test", cut, to, {}});
        } else {
            runs.push_back({"", output, from, to, {}});
        }
        for (auto& run : runs)
            for (Millis latency : parse_latencies(latencies)) {
                Config c = base;
                c.latency_ms = latency;
                run.engines.push_back(std::make_unique<Engine>(c));
            }
        std::string source = candles ? "candles" : live ? "live" : "events";
        auto write_all = [&] {
            for (const auto& run : runs)
                for (const auto& e : run.engines)
                    write_results(*e, run.engines.size() == 1 ? run.directory
                                      : run.directory + "/latency-" + std::to_string(e->config().latency_ms) + "ms", source);
        };
        auto feed = [&](const Event& e, bool decide) {
            for (auto& run : runs)
                if (e.time >= run.from && e.time < run.to)
                    for (const auto& engine : run.engines) engine->on_event(e, decide);
        };

        if (candles) {
            const auto data = btc::backtest::load(spot_csv, perp_csv, funding_csv);
            for (const auto& [t, p] : data.prices) {
                Event s{t * 1000, 'Q', Spot, p.spot_open, 0, p.spot_open, 0, 0, 0};
                Event f{t * 1000, 'Q', Perp, p.perp_open, 0, p.perp_open, 0, 0, 0};
                feed(s, false);
                feed(f, true);
            }
        } else if (live) {
            EventReader reader(std::cin, perp_product, spot_product);
            Event e;
            Millis next_write = 0;
            const auto& engines = runs.front().engines;
            while (reader.next(e)) {
                feed(e, true);
                if (e.time >= next_write) {
                    write_all();
                    next_write = (e.time / 60000 + 1) * 60000;
                    const auto& first = *engines.front();
                    const auto b = first.mid_basis_bps();
                    std::cerr << utc(e.time) << " basis=" << (b ? std::to_string(*b) : std::string("n/a"))
                              << " center=" << first.center_bps() << " proj_funding_bps=" << first.projected_funding_bps();
                    for (const auto& engine : engines)
                        std::cerr << " | " << engine->config().latency_ms << "ms net=" << engine->totals().net()
                                  << " pos=" << engine->position_contracts() << " fills=" << engine->fills().size();
                    std::cerr << '\n';
                }
            }
        } else {
            for (const auto& name : event_files) {
                std::ifstream file;
                if (name != "-") { file.open(name); if (!file) throw std::runtime_error("Cannot read " + name); }
                EventReader reader(name == "-" ? std::cin : file, perp_product, spot_product);
                // One-event lookahead only to batch events that share a timestamp.
                Event current, upcoming;
                bool have = reader.next(current);
                while (have) {
                    const bool more = reader.next(upcoming);
                    feed(current, !(more && upcoming.time == current.time));
                    current = upcoming; have = more;
                }
                if (reader.skipped()) std::cerr << name << ": skipped " << reader.skipped() << " malformed rows\n";
            }
        }
        for (const auto& run : runs) for (const auto& e : run.engines) e->finish();
        write_all();
        for (const auto& run : runs) {
            if (!run.name.empty()) {
                const auto& eq = run.engines.front()->equity();
                std::cout << "== " << run.name << " " << (eq.empty() ? "" : utc(eq.front().time)) << " .. "
                          << (eq.empty() ? "" : utc(eq.back().time)) << '\n';
            }
            report(run.engines);
        }
        if (base.ema_halflife_min > 0)
            std::cout << "Fair basis: causal EMA, half-life " << base.ema_halflife_min
                      << " min (no quotes/trades during the first half-life).\n";
        const auto& c = base;
        if (c.perp_taker_bps == 0 && c.perp_maker_bps == 0 && c.perp_fee_usd_per_contract == 0 && c.spot_taker_bps == 0)
            std::cout << "WARNING: all fees are zero. Results are not net of trading costs.\n";
        std::cout << "Results in " << output << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
}
