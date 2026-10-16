#include "basis-maker.h"
#include <cmath>
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
        std::string events = "../data/binance/maker/2023-07-20/events-spot-trades.csv";
        std::string output = "results/binance-basis-maker";
        for (int i = 1; i < argc; ++i) {
            const std::string key = argv[i];
            if (key == "--help") {
                std::cout << "Paired basis entries: equilibrium -5 bps, join perp bid below / ask above, immediate equal-BTC spot hedge.\n"
                    << "No exits; zero funding cash flows; zero latency. Entry quotes cancel before funding.\n"
                    << "  --events FILE --output DIR\n"
                    << "  --equilibrium-bps -5 --order-btc 0.01 --max-gross-perp-btc 0.05\n"
                    << "  --perp-maker-bps 2 --spot-taker-bps 10\n"
                    << "  --funding-cancel-seconds 60 --spot-half-spread-bps 0.5 (price-proxy input only)\n"
                    << "  --participation 0.1 --fill-at-touch 1 (queue approximation)\n"
                    << "  --quantity-step 0.001 --tick 0.1\n";
                return 0;
            }
            if (++i >= argc) throw std::runtime_error("Missing value for "+key);
            const std::string value = argv[i];
            if (key == "--events") events = value;
            else if (key == "--output") output = value;
            else if (key == "--equilibrium-bps") c.equilibrium_bps = number(value);
            else if (key == "--order-btc") c.order_btc = number(value);
            else if (key == "--max-gross-perp-btc") c.max_gross_perp_btc = number(value);
            else if (key == "--perp-maker-bps") c.perp_maker_bps = number(value);
            else if (key == "--spot-taker-bps") c.spot_taker_bps = number(value);
            else if (key == "--spot-half-spread-bps") c.proxy_spot_half_spread_bps = number(value);
            else if (key == "--participation") c.participation = number(value);
            else if (key == "--quantity-step") c.quantity_step = number(value);
            else if (key == "--tick") c.tick = number(value);
            else if (key == "--funding-cancel-seconds") {
                const double seconds = number(value);
                if (seconds < 0 || seconds > 3600) throw std::runtime_error("Cancellation buffer must be in [0,3600] seconds");
                c.funding_cancel_ms = static_cast<btc::basis_maker::Millis>(seconds*1000);
            } else if (key == "--fill-at-touch") {
                if (value != "0" && value != "1") throw std::runtime_error("fill-at-touch must be 0 or 1");
                c.fill_at_touch = value == "1";
            } else throw std::runtime_error("Unknown argument: "+key);
        }
        btc::basis_maker::Engine engine(c);
        btc::basis_maker::replay(engine,events);
        btc::basis_maker::write_results(engine,output);
        const auto t = engine.totals();
        std::cout << "Basis maker: paired_partial_fills=" << engine.pairs().size() << " price_pnl=" << t.price_pnl
                  << " fees=" << t.fees << " net_mtm=" << t.net() << " perp_btc=" << t.perp_btc
                  << " spot_btc=" << t.spot_btc << " delta_btc=" << t.perp_btc+t.spot_btc << '\n'
                  << "No exits; funding assumed zero. Results in " << output << '\n';
        return 0;
    } catch (const std::exception& e) { std::cerr << "error: " << e.what() << '\n'; return 1; }
}
