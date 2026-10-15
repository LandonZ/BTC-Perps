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
    "                          maker: quote the perp around spot theo, no hedge (cop and lean comes later)\n"
    "  --latency-ms 5,50,250   one run per latency (default 5,50,250 = best/middle/worst; candles: 0)\n"
    "  --threshold-bps 5       taker entry band / maker quote half-width\n"
    "  --center-bps 0          fixed fair basis, or --ema-halflife-min N for a causal rolling center\n"
    "  --contracts 1           0.01 BTC each   --hold-minutes 5   --profit-exit 1\n"
    "  --max-inventory 5 --skew-bps 1 --fill-at-touch 0   (maker)\n"
    "Costs (default 0 -- set your fee tier!):\n"
    "  --perp-taker-bps --perp-maker-bps --perp-fee-usd (per contract) --spot-taker-bps\n"
    "Other: --prev-funding-bps 0 --from-ms T --to-ms T (train/test windows) --output DIR\n"
    "  --perp-product BIP-20DEC30-CDE --spot-product BTC-USD\n";

std::vector<btc::paper::Millis> parse_latencies(const std::string& text) {
    std::vector<Millis> out;
    std::stringstream in(text);
    for (std::string part; std::getline(in, part, ',');) out.push_back(static_cast<Millis>(number(part)));
    if (out.empty()) throw std::runtime_error("No latencies");
    return out;
}

void report(const std::vector<std::unique_ptr<Engine>>& engines) {
    std::cout << std::fixed << std::setprecision(4)
              << "latency_ms  trades   win%     net_usd    fees_usd  funding_usd  mean_bps  entry_slip_bps    max_dd  position\n";
    for (const auto& e : engines) {
        const auto& trips = e->trips();
        int wins = 0; double bps = 0, slip = 0;
        for (const auto& t : trips) {
            wins += t.net_pnl > 0; bps += t.net_bps;
            slip += t.direction > 0 ? t.entry_basis_bps - t.signal_basis_bps : t.signal_basis_bps - t.entry_basis_bps;
        }
        const auto n = trips.size();
        const auto total = e->totals();
        const bool maker = e->config().mode == Mode::Maker;
        std::cout << std::setw(10) << e->config().latency_ms << std::setw(8) << (maker ? e->fills().size() : n)
                  << std::setw(7) << std::setprecision(1) << (n ? 100.0 * wins / n : 0) << std::setprecision(4)
                  << std::setw(12) << total.net() << std::setw(12) << total.fees << std::setw(13) << total.funding_pnl
                  << std::setw(10) << (n ? bps / n : 0) << std::setw(16) << (n ? slip / n : 0)
                  << std::setw(10) << e->max_drawdown() << std::setw(10) << e->position_contracts() << '\n';
    }
    if (!engines.empty() && engines.front()->config().mode == Mode::Maker)
        std::cout << "(maker: the trades column counts perp fills; net includes inventory marked at the perp mid)\n";
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
            else if (option == "--ema-halflife-min") base.ema_halflife_min = number(value);
            else if (option == "--contracts") base.contracts = static_cast<int>(number(value));
            else if (option == "--hold-minutes") base.max_hold_ms = static_cast<Millis>(number(value) * 60000);
            else if (option == "--profit-exit") base.exit_on_profit = flag(value);
            else if (option == "--max-inventory") base.max_inventory_contracts = static_cast<int>(number(value));
            else if (option == "--skew-bps") base.skew_bps_per_contract = number(value);
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

        // Candle quotes change once a minute, so a latency would just mean "fill at the next open".
        if (latencies.empty()) latencies = candles ? "0" : "5,50,250";
        std::vector<std::unique_ptr<Engine>> engines;
        for (Millis latency : parse_latencies(latencies)) {
            Config c = base;
            c.latency_ms = latency;
            engines.push_back(std::make_unique<Engine>(c));
        }
        auto directory = [&](const Engine& e) {
            return engines.size() == 1 ? output : output + "/latency-" + std::to_string(e.config().latency_ms) + "ms";
        };
        std::string source = candles ? "candles" : live ? "live" : "events";
        auto write_all = [&] { for (const auto& e : engines) write_results(*e, directory(*e), source); };
        auto feed = [&](const Event& e, bool decide) {
            if (e.time < from || e.time >= to) return;
            for (const auto& engine : engines) engine->on_event(e, decide);
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
                                  << " pos=" << engine->position_contracts() << " trades=" << engine->trips().size();
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
        for (const auto& e : engines) e->finish();
        write_all();
        report(engines);
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
