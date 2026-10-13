#include "../model/theo-model.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void check(bool good,const char* message) { if (!good) throw std::runtime_error(message); }
void close(double actual,double expected,const char* message) {
    if (std::abs(actual-expected) > 1e-8) {
        std::cerr << message << ": actual=" << actual << ", expected=" << expected << '\n';
        throw std::runtime_error(message);
    }
}
}
int main() {
    using namespace btc::theo;
    try {
        close(mark_price(100,102,101),101,"Median last trade inside book");
        close(mark_price(100,102,98),100,"Median is bid when last trade below book");
        close(mark_price(100,102,104),102,"Median is ask when last trade above book");
        close(mark_price(100,100,101),100,"Allow a zero-spread scenario");
        close(next_funding_bps(24,1),1,"Divide one-hour average gap by 24 before smoothing");
        close(next_funding_bps(0,1),0.25,"Previous positive funding persists with zero current gap");
        close(next_funding_bps(0,0),0,"Zero is the steady funding-neutral gap");
        close(next_funding_bps(-5,0),-0.15625,"Minus five bps is not the funding zero for this formula");
        close(next_funding_bps(-8,1),0,"Transient zero depends on previous funding");

        BasisInputs in;
        in.index = 100; in.spot_bid = 99; in.spot_ask = 101;
        in.perp_bid = 101; in.perp_ask = 103; in.last_trade = 102;
        in.twap_basis_bps = 24; in.previous_funding_bps = 1;
        in.exit_basis_bps = 0; in.horizon_minutes = 60; in.minutes_to_next_funding = 60;
        BasisCosts c;
        c.quantity_btc = 1; c.round_trip_fee_bps = 0; c.slippage_bps_per_fill = 0;
        c.exit_spot_half_spread_bps = c.exit_perp_half_spread_bps = 0;
        c.financing_apr = c.btc_borrow_apr = c.min_net_edge_bps = 0;
        auto r = basis_theo(in,c);
        close(r.mark_index_basis_bps,200,"Mark/index gap differs from its historical TWAP");
        close(r.long_funding_paid_per_btc,0.0102,"Funding cash payment uses mark times fractional rate");
        close(r.theo,99.9898,"Theo equals expected exit perp value minus long funding payments");
        close(r.long_perp_price_pnl,-4,"Long-perp hedge includes actual entry ask and spot bid");
        close(r.short_perp_price_pnl,0,"Short-perp hedge includes actual entry bid and spot ask");
        close(r.long_perp_net_pnl,-4.0102,"Positive funding debits the long perp pair");
        close(r.short_perp_net_pnl,0.0102,"Positive funding credits the short perp pair");
        check(std::string(r.candidate) == "SHORT_PERP_LONG_SPOT","Choose the profitable funded short-perp pair");

        c.round_trip_fee_bps = 2;
        r = basis_theo(in,c);
        close(r.fees,0.02,"All four fees total TWO bps, charged once");
        close(r.short_perp_net_pnl,-0.0098,"Fees can erase a funding receipt");
        check(std::string(r.candidate) == "NEITHER","No trade when neither net edge is positive");
        in.horizon_minutes = 59;
        r = basis_theo(in,c);
        check(r.funding_rates_bps.empty(),"No payment before hourly boundary");
        close(r.long_funding_paid_per_btc,0,"Cannot collect funding outside holding window");

        in.horizon_minutes = 180; in.previous_funding_bps = 0;
        r = basis_theo(in,c);
        check(r.funding_rates_bps.size() == 3,"Include exit funding boundary");
        close(r.funding_rates_bps[0],0.75,"First recursively smoothed rate");
        close(r.funding_rates_bps[1],0.9375,"Second recursively smoothed rate");
        close(r.funding_rates_bps[2],0.984375,"Third recursively smoothed rate");
        close(r.long_funding_paid_per_btc,102*2.671875/10000,"Funding sum agrees with geometric-series result");

        in.perp_bid = 97; in.perp_ask = 99; in.last_trade = 98;
        in.previous_funding_bps = -1; in.twap_basis_bps = -24; in.horizon_minutes = 60;
        c.round_trip_fee_bps = 0;
        r = basis_theo(in,c);
        close(r.long_perp_net_pnl,0.0098,"Negative funding credits the long-perp pair");
        check(std::string(r.candidate) == "LONG_PERP_SHORT_SPOT","Negative funding can favor long perp / short spot");

        // Verify cash-flow identities and break-even entry bounds in a full-cost scenario.
        in.index = 100000; in.spot_bid = 99990; in.spot_ask = 100010;
        in.perp_bid = 100190; in.perp_ask = 100210; in.last_trade = 100200;
        in.twap_basis_bps = 20; in.previous_funding_bps = 0.5;
        in.exit_basis_bps = 20; in.horizon_minutes = 1440;
        c = BasisCosts{};
        c.round_trip_fee_bps = 2; c.slippage_bps_per_fill = 0.5;
        c.exit_spot_half_spread_bps = c.exit_perp_half_spread_bps = 1;
        c.financing_apr = 0.05; c.btc_borrow_apr = 0.10; c.min_net_edge_bps = 1;
        const auto base = basis_theo(in,c);
        close(base.long_perp_net_pnl,base.long_perp_price_pnl-c.quantity_btc*base.long_funding_paid_per_btc-
              base.fees-base.financing-base.btc_borrow,"Long pair cost/funding reconciliation");
        close(base.short_perp_net_pnl,base.short_perp_price_pnl+c.quantity_btc*base.long_funding_paid_per_btc-
              base.fees-base.financing,"Short pair cost/funding reconciliation");
        const double slip = c.slippage_bps_per_fill/10000;
        const double exit_perp_ask = 100200*(1+(c.exit_perp_half_spread_bps+c.slippage_bps_per_fill)/10000);
        const double exit_spot_bid = 100000*(1-(c.exit_spot_half_spread_bps+c.slippage_bps_per_fill)/10000);
        const double threshold_short_profit = c.quantity_btc*(base.minimum_short_perp_bid*(1-slip)-
            exit_perp_ask+exit_spot_bid-in.spot_ask*(1+slip)+base.long_funding_paid_per_btc)-base.fees-base.financing;
        close(threshold_short_profit,c.quantity_btc*100000*c.min_net_edge_bps/10000,
              "Short entry bound exactly covers costs and requested minimum edge under fixed forecasts");
        const double exit_perp_bid = 100200*(1-(c.exit_perp_half_spread_bps+c.slippage_bps_per_fill)/10000);
        const double exit_spot_ask = 100000*(1+(c.exit_spot_half_spread_bps+c.slippage_bps_per_fill)/10000);
        const double threshold_long_profit = c.quantity_btc*(exit_perp_bid-base.maximum_long_perp_ask*(1+slip)+
            in.spot_bid*(1-slip)-exit_spot_ask-base.long_funding_paid_per_btc)-base.fees-base.financing-base.btc_borrow;
        close(threshold_long_profit,c.quantity_btc*100000*c.min_net_edge_bps/10000,
              "Long entry bound covers costs, BTC borrowing and requested edge");
        close(base.financing,0.01*(100000+100200*0.2)*0.05/365,"Capital financing APR scales to a day");
        close(base.btc_borrow,0.01*100000*0.10/365,"BTC borrowing applies only to the long-perp pair");
        check(base.short_perp_net_pnl > 0,"Synthetic unchanged-basis carry scenario is positive");
        in.exit_basis_bps = 40;
        const auto wider = basis_theo(in,c);
        check(wider.short_perp_net_pnl < 0,"A wider exit basis can erase funding profits");
        close(wider.long_funding_paid_per_btc,base.long_funding_paid_per_btc,
              "Changing terminal basis does not secretly change the supplied funding path");

        // Active zero-cost model: basis changes and funding fully explain net PnL.
        c = BasisCosts{};
        in.spot_bid = in.spot_ask = 100000;
        in.perp_bid = in.perp_ask = in.last_trade = 100200;
        in.exit_basis_bps = 20;
        const auto zero = basis_theo(in,c);
        close(zero.fees+zero.financing+zero.btc_borrow,0,"Default fee and carry charges are zero");
        close(zero.long_price_pnl_bps,0,"Unchanged basis at zero costs has no price PnL");
        close(zero.short_price_pnl_bps,0,"Unchanged short basis has no price PnL");
        close(zero.long_net_pnl_bps,-zero.long_funding_paid_bps,"Long basis pays projected positive funding");
        close(zero.short_net_pnl_bps,zero.long_funding_paid_bps,"Short basis receives projected positive funding");
        in.exit_basis_bps = 40;
        const auto zero_wider = basis_theo(in,c);
        close(zero_wider.long_price_pnl_bps,20,"Buying basis earns its twenty-bp increase");
        close(zero_wider.short_price_pnl_bps,-20,"Selling basis loses its twenty-bp increase");
        close(zero_wider.long_net_pnl_bps,20-zero.long_funding_paid_bps,"Basis change minus funding, without costs");
        in.horizon_minutes = 5; in.exit_basis_bps = 0;
        const auto convergence = basis_theo(in,c);
        close(convergence.short_net_pnl_bps,20,"Selling at twenty and exiting at zero earns twenty bps before funding");
        close(convergence.long_net_pnl_bps,-20,"Opposite basis direction loses twenty bps");
        in.perp_bid = in.perp_ask = in.last_trade = 99800;
        in.twap_basis_bps = -20; in.previous_funding_bps = -0.5;
        in.exit_basis_bps = -20; in.horizon_minutes = 1440;
        const auto negative = basis_theo(in,c);
        check(negative.long_net_pnl_bps > 0 && negative.short_net_pnl_bps < 0,
              "Negative funding rewards buying unchanged basis with zero costs");
        std::cout << "PASS: reference funding formula, mark median, smoothing, payment timing,\n"
            << "both basis trades, aggregate fees, financing/borrow, entry bounds and exit-basis risk.\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
