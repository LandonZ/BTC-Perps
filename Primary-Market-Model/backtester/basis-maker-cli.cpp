#include "basis-maker.h"
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
double number(const std::string& s) {
    std::size_t used = 0; const double v = std::stod(s,&used);
    if (used != s.size() || !std::isfinite(v)) throw std::runtime_error("Invalid number: "+s);
    return v;
}
}
int main(int argc,char** argv) {
    try {
        btc::basis_maker::Config c;
        std::vector<std::string> events;
        std::string premium;
        std::string output = "results/work";
        bool details = false;
        for (int i = 1; i < argc; ++i) {
            const std::string key = argv[i];
            if (key == "--help") {
                std::cout << "Paired basis entries: equilibrium -5 bps, bid at/below -6.5 / ask at/above -3.5 bps, immediate equal-BTC spot hedge.\n"
                    << "Hold through convergence; exit any expected funding payer before settlement, including the flat region.\n"
                    << "  --events FILE (repeatable) --events-list FILE --output DIR --details 0|1\n"
                    << "  --equilibrium-bps -5 --entry-offset-bps 1.5 --order-btc 0.01 --max-gross-perp-btc 0 (unlimited)\n"
                    << "  --premium-events FILE (required for pre-funding exits)\n"
                    << "  --exit-on-equilibrium 0 --exit-before-funding 1 --funding-cashflows 1\n"
                    << "  --funding-exit-payers-only 1 --funding-interest-bps 1 --funding-clamp-bps 5\n"
                    << "  --cycle-fee-bps VALUE (optional comparison; splits equally across all four fills)\n"
                    << "  --perp-maker-bps 0 --perp-taker-bps 0.5 --spot-taker-bps 0.5 (defaults, bps per fill)\n"
                    << "  --funding-cancel-seconds 60 --spot-half-spread-bps 0.5 (price-proxy input only)\n"
                    << "  --participation 0.1 --fill-at-touch 1 (queue approximation)\n"
                    << "  --quantity-step 0.001 --tick 0.1\n"
                    << "  --center-halflife-seconds 0 (>0: fair basis is an EMA of the mid basis instead of --equilibrium-bps)\n"
                    << "  --max-hold-seconds 0 (none) --close-at-end 0|1 --funding-exit-min-bps 0\n";
                return 0;
            }
            if (++i >= argc) throw std::runtime_error("Missing value for "+key);
            const std::string value = argv[i];
            if (key == "--events") events.push_back(value);
            else if (key == "--premium-events") premium = value;
            else if (key == "--events-list") {
                std::ifstream list(value);
                if (!list) throw std::runtime_error("Cannot open events list: "+value);
                std::string path;
                while (std::getline(list,path)) {
                    if (!path.empty() && path.back() == '\r') path.pop_back();
                    if (!path.empty()) events.push_back(path);
                }
            }
            else if (key == "--output") output = value;
            else if (key == "--details") {
                if (value != "0" && value != "1") throw std::runtime_error("details must be 0 or 1");
                details = value == "1";
            }
            else if (key == "--equilibrium-bps") c.equilibrium_bps = number(value);
            else if (key == "--entry-offset-bps") c.entry_offset_bps = number(value);
            else if (key == "--order-btc") c.order_btc = number(value);
            else if (key == "--max-gross-perp-btc") c.max_gross_perp_btc = number(value);
            else if (key == "--cycle-fee-bps") {
                const double per_fill = number(value)/4;
                c.perp_maker_bps = per_fill; c.perp_taker_bps = per_fill; c.spot_taker_bps = per_fill;
            }
            else if (key == "--perp-maker-bps") c.perp_maker_bps = number(value);
            else if (key == "--perp-taker-bps") c.perp_taker_bps = number(value);
            else if (key == "--spot-taker-bps") c.spot_taker_bps = number(value);
            else if (key == "--spot-half-spread-bps") c.proxy_spot_half_spread_bps = number(value);
            else if (key == "--funding-interest-bps") c.funding_interest_bps = number(value);
            else if (key == "--funding-clamp-bps") c.funding_clamp_bps = number(value);
            else if (key == "--exit-before-funding" || key == "--funding-cashflows" || key == "--funding-exit-payers-only") {
                if (value != "0" && value != "1") throw std::runtime_error(key+" must be 0 or 1");
                if (key == "--exit-before-funding") c.exit_before_funding = value == "1";
                if (key == "--funding-cashflows") c.funding_cashflows = value == "1";
                if (key == "--funding-exit-payers-only") c.funding_exit_payers_only = value == "1";
            }
            else if (key == "--participation") c.participation = number(value);
            else if (key == "--quantity-step") c.quantity_step = number(value);
            else if (key == "--tick") c.tick = number(value);
            else if (key == "--funding-cancel-seconds") {
                const double seconds = number(value);
                if (seconds < 0 || seconds > 3600) throw std::runtime_error("Cancellation buffer must be in [0,3600] seconds");
                c.funding_cancel_ms = static_cast<btc::basis_maker::Millis>(seconds*1000);
            } else if (key == "--exit-on-equilibrium") {
                if (value != "0" && value != "1") throw std::runtime_error("exit-on-equilibrium must be 0 or 1");
                c.exit_on_equilibrium = value == "1";
            } else if (key == "--center-halflife-seconds" || key == "--max-hold-seconds") {
                const double seconds = number(value);
                if (seconds < 0) throw std::runtime_error(key+" must be nonnegative");
                (key == "--center-halflife-seconds" ? c.center_halflife_ms : c.max_hold_ms) = static_cast<btc::basis_maker::Millis>(seconds*1000);
            } else if (key == "--close-at-end") {
                if (value != "0" && value != "1") throw std::runtime_error("close-at-end must be 0 or 1");
                c.close_at_end = value == "1";
            } else if (key == "--funding-exit-min-bps") c.funding_exit_min_bps = number(value);
            else if (key == "--fill-at-touch") {
                if (value != "0" && value != "1") throw std::runtime_error("fill-at-touch must be 0 or 1");
                c.fill_at_touch = value == "1";
            } else throw std::runtime_error("Unknown argument: "+key);
        }
        if (events.empty()) throw std::runtime_error("Supply --events or --events-list for CSV replay; use make run for the current archive backtest");
        if (c.exit_before_funding && premium.empty()) throw std::runtime_error("Pre-funding exits require --premium-events; future settlement rates cannot substitute");
        c.retain_decisions = details; c.retain_fills = details;
        btc::basis_maker::Engine engine(c);
        btc::basis_maker::replay_files(engine,events,premium);
        btc::basis_maker::write_results(engine,output,details);
        const auto t = engine.totals();
        std::cout << "Basis maker: paired_partial_fills=" << engine.pairs().size() << " price_pnl=" << t.price_pnl
                  << " fees=" << t.fees << " realized=" << t.realized_pnl << " net_mtm=" << t.net() << " perp_btc=" << t.perp_btc
                  << " spot_btc=" << t.spot_btc << " delta_btc=" << t.perp_btc+t.spot_btc << '\n'
                  << "funding_pnl=" << t.funding_pnl << "; results in " << output << '\n';
        return 0;
    } catch (const std::exception& e) { std::cerr << "error: " << e.what() << '\n'; return 1; }
}
