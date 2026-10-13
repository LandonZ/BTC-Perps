#include "theo-model.h"
#include <cmath>
#include <iomanip>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

namespace {
double number(const std::string& s) {
    std::size_t n = 0; const double v = std::stod(s,&n);
    if (n != s.size() || !std::isfinite(v)) throw std::runtime_error("Invalid number: " + s);
    return v;
}
void help() {
    std::cout << "BTC basis theo using the supplied smoothed hourly funding formula\n"
        << "Run --demo for a synthetic five-minute long-basis scenario. Custom inputs:\n"
        << "  --spot PRICE --perp PRICE --twap-basis-bps X --previous-funding-bps X\n"
        << "  Or --spot-bid X --spot-ask Y --perp-bid X --perp-ask Y --last PRICE\n"
        << "  --index PRICE (defaults to spot midpoint)\n"
        << "  --horizon-minutes 5 --minutes-to-funding 60 --exit-basis-bps -5\n"
        << "  Exit target defaults to -5 bps for a discount, +5 bps for a premium.\n"
        << "Defaults: ALL trading/carry costs = 0; funding remains included.\n"
        << "Optional costs: --qty-btc 0.01 --round-trip-fee-bps 0 --slippage-bps 0\n"
        << "  --spot-half-spread-bps 0 --perp-half-spread-bps 0\n"
        << "  --financing-apr 0 --btc-borrow-apr 0 --margin 0.20 --min-edge-bps 0\n"
        << "TWAP basis is the 1-hour average mark/index gap in bps BEFORE dividing by 24.\n"
        << "Funding and APR signs: positive funding is paid by long perps; APR is decimal.\n"
        << "The model compares paired basis trades, with conditional PnL, not locked arbitrage.\n";
}
}
int main(int argc,char** argv) {
    try {
        btc::theo::BasisInputs in; btc::theo::BasisCosts costs;
        std::optional<double> spot_mid,perp_mid,index,last,exit_basis;
        double spot_half = 0,perp_half = 0;
        bool demo = false,twap_given = false,previous_given = false;
        bool spot_bid_given = false,spot_ask_given = false,perp_bid_given = false,perp_ask_given = false;
        for (int i = 1; i < argc; ++i) if (std::string(argv[i]) == "--demo") demo = true;
        if (demo) {
            in.index = 100000; spot_mid = 100000; perp_mid = 99900;
            in.twap_basis_bps = -10; in.previous_funding_bps = -0.05;
            in.horizon_minutes = 5; in.minutes_to_next_funding = 60;
        }
        for (int i = 1; i < argc; ++i) {
            const std::string key = argv[i];
            if (key == "--help") { help(); return 0; }
            if (key == "--demo") continue;
            if (++i >= argc) throw std::runtime_error("Missing value for " + key);
            const double v = number(argv[i]);
            if (key == "--spot") spot_mid = v;
            else if (key == "--perp") perp_mid = v;
            else if (key == "--index") index = v;
            else if (key == "--last") last = v;
            else if (key == "--spot-bid") { in.spot_bid = v; spot_bid_given = true; }
            else if (key == "--spot-ask") { in.spot_ask = v; spot_ask_given = true; }
            else if (key == "--perp-bid") { in.perp_bid = v; perp_bid_given = true; }
            else if (key == "--perp-ask") { in.perp_ask = v; perp_ask_given = true; }
            else if (key == "--twap-basis-bps") { in.twap_basis_bps = v; twap_given = true; }
            else if (key == "--previous-funding-bps") { in.previous_funding_bps = v; previous_given = true; }
            else if (key == "--exit-basis-bps") exit_basis = v;
            else if (key == "--horizon-hours") in.horizon_minutes = v*60;
            else if (key == "--horizon-minutes") in.horizon_minutes = v;
            else if (key == "--minutes-to-funding") in.minutes_to_next_funding = v;
            else if (key == "--qty-btc") costs.quantity_btc = v;
            else if (key == "--round-trip-fee-bps") costs.round_trip_fee_bps = v;
            else if (key == "--slippage-bps") costs.slippage_bps_per_fill = v;
            else if (key == "--spot-half-spread-bps") spot_half = costs.exit_spot_half_spread_bps = v;
            else if (key == "--perp-half-spread-bps") perp_half = costs.exit_perp_half_spread_bps = v;
            else if (key == "--financing-apr") costs.financing_apr = v;
            else if (key == "--btc-borrow-apr") costs.btc_borrow_apr = v;
            else if (key == "--margin") costs.perp_margin_fraction = v;
            else if (key == "--min-edge-bps") costs.min_net_edge_bps = v;
            else throw std::runtime_error("Unknown option: " + key);
        }
        if (demo && spot_bid_given) spot_mid.reset();
        if (demo && perp_bid_given) perp_mid.reset();
        if (spot_bid_given != spot_ask_given || perp_bid_given != perp_ask_given)
            throw std::runtime_error("Provide both bid and ask for each supplied book");
        if ((spot_mid && spot_bid_given) || (perp_mid && perp_bid_given))
            throw std::runtime_error("Choose midpoint or bid/ask inputs for each market, not both");
        if (spot_half < 0 || perp_half < 0 || spot_half >= 10000 || perp_half >= 10000)
            throw std::runtime_error("Invalid half-spread assumption");
        if (spot_mid) { in.spot_bid = *spot_mid*(1-spot_half/10000); in.spot_ask = *spot_mid*(1+spot_half/10000); }
        if (perp_mid) { in.perp_bid = *perp_mid*(1-perp_half/10000); in.perp_ask = *perp_mid*(1+perp_half/10000); }
        if (!demo && (!twap_given || !previous_given))
            throw std::runtime_error("Supply the TWAP basis and previous funding rate; they are not inferred from one snapshot");
        if (in.horizon_minutes <= 0 || in.horizon_minutes > 5)
            throw std::runtime_error("The current basis strategy requires a positive horizon of at most five minutes");
        const double spot = (in.spot_bid+in.spot_ask)/2,perp = (in.perp_bid+in.perp_ask)/2;
        if (index) in.index = *index; else if (!demo || spot_mid || spot_bid_given) in.index = spot;
        if (last) in.last_trade = *last; else if (!demo || perp_mid || perp_bid_given) in.last_trade = perp;
        in.exit_basis_bps = exit_basis.value_or(perp < spot ? -5 : 5);
        const auto r = btc::theo::basis_theo(in,costs);
        const double quantity = costs.quantity_btc;
        std::cout << std::fixed << std::setprecision(6)
            << (demo ? "SYNTHETIC SCENARIO\n" : "SUPPLIED SCENARIO\n")
            << "Mark = median(bid,ask,last):   " << r.mark << '\n'
            << "Current mark/index gap (bps): " << r.mark_index_basis_bps << '\n'
            << "Current hedge basis (bps):    " << r.market_basis_bps << '\n'
            << "One-hour TWAP basis (bps):    " << in.twap_basis_bps << '\n'
            << "Previous hourly funding:      " << in.previous_funding_bps << " bps\n"
            << "NEXT HOURLY FUNDING:          " << r.next_funding_bps << " bps\n"
            << "Horizon / payments:           " << in.horizon_minutes/60 << " hours / " << r.funding_rates_bps.size() << '\n'
            << "Assumed exit basis (bps):     " << in.exit_basis_bps << '\n'
            << "Cumulative long funding/BTC:  " << r.long_funding_paid_per_btc << '\n'
            << "THEO (before trading costs):  " << r.theo << '\n'
            << "Maximum long-perp entry ask:  " << r.maximum_long_perp_ask << '\n'
            << "Minimum short-perp entry bid: " << r.minimum_short_perp_bid << '\n'
            << "Long-basis entry ceiling:     " << (r.maximum_long_perp_ask/spot-1)*10000 << " bps\n"
            << "Short-basis entry floor:      " << (r.minimum_short_perp_bid/spot-1)*10000 << " bps\n"
            << "Quantity:                    " << quantity << " BTC per leg\n"
            << "Total fee assumption:         " << costs.round_trip_fee_bps << " bps / " << r.fees << '\n'
            << "Capital financing:            " << r.financing << '\n'
            << "BTC borrow (long-perp pair):  " << r.btc_borrow << '\n'
            << "\nLONG PERP / SHORT SPOT\n"
            << "  Basis/price PnL (bps):      " << r.long_price_pnl_bps << '\n'
            << "  Funding contribution (bps): " << -r.long_funding_paid_bps << '\n'
            << "  EXPECTED NET PnL (bps):     " << r.long_net_pnl_bps << '\n'
            << "  Price PnL after execution:   " << r.long_perp_price_pnl << '\n'
            << "  Funding cash flow:          " << -quantity*r.long_funding_paid_per_btc << '\n'
            << "  EXPECTED NET PnL:           " << r.long_perp_net_pnl << '\n'
            << "\nSHORT PERP / LONG SPOT\n"
            << "  Basis/price PnL (bps):      " << r.short_price_pnl_bps << '\n'
            << "  Funding contribution (bps): " << r.long_funding_paid_bps << '\n'
            << "  EXPECTED NET PnL (bps):     " << r.short_net_pnl_bps << '\n'
            << "  Price PnL after execution:   " << r.short_perp_price_pnl << '\n'
            << "  Funding cash flow:          " << quantity*r.long_funding_paid_per_btc << '\n'
            << "  EXPECTED NET PnL:           " << r.short_perp_net_pnl << '\n'
            << "\nBASIS STRATEGY ENTRY:         "
            << (r.market_basis_bps < -5-1e-10 && r.long_net_pnl_bps > 0 ? "LONG_PERP_SHORT_SPOT" :
                r.market_basis_bps > 5+1e-10 && r.short_net_pnl_bps > 0 ? "SHORT_PERP_LONG_SPOT" : "WAIT") << '\n'
            << "Buy below -5 bps; sell above +5 bps, with positive forecast edge for that side.\n"
            << "\nPnL bps are normalized to entry spot notional. Zero-cost defaults retain funding.\n"
            << "\nFuture TWAP, settlement mark and spot reference are held constant in this scenario.\n"
            << "Exit basis is supplied, not guaranteed. Positive PnL is conditional, not locked arbitrage.\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "Error: " << e.what() << '\n'; return 1; }
}
