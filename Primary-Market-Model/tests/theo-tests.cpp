#include "../model/theo-model.h"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void close(double actual, double expected, const char* message) {
    if (std::abs(actual-expected) > 1e-8) {
        std::cerr << message << ": actual=" << actual << ", expected=" << expected << '\n';
        throw std::runtime_error(message);
    }
}
}
int main() {
    using namespace btc::theo;
    try {
        Inputs in;
        in.spot = 100000; in.expected_exit_basis_bps = 1;
        in.expected_funding_paid_bps = 0.05;
        Config c;
        close(c.round_trip_fee_bps+c.round_trip_hedge_cost_bps,0,"Default trading costs are zero");
        c.round_trip_fee_bps = 2; c.round_trip_hedge_cost_bps = 2;
        auto q = price(in, c);
        close(q.theo,100009.5,"Theo = spot + exit basis - cumulative funding paid");
        close(q.half_width_bps,2.5,"Total two-bps fees are included once in the full spread");
        close(q.bid,99984.5,"Bid subtracts half the complete-cycle budget");
        close(q.ask,100034.5,"Ask adds half the complete-cycle budget");
        close((q.ask-q.bid)/in.spot*bps,5,"Full spread = two fee + two hedge + one target profit bps");

        in.inventory_btc = 0.05;
        q = price(in,c);
        close(q.theo,100009.5,"Inventory does not change underlying theo");
        close(q.reservation_price,100007.5,"Positive long inventory shifts quotes lower");
        close(q.bid,99982.5,"Inventory-adjusted demo bid");
        close(q.ask,100032.5,"Inventory-adjusted demo ask");
        in.inventory_btc = -0.05;
        close(price(in,c).reservation_price,100011.5,"Short inventory shifts quotes higher");
        in.inventory_btc = 0;
        in.expected_funding_paid_bps = -0.05;
        close(price(in,c).theo,100010.5,"Funding received raises long reservation value");
        in.expected_funding_paid_bps = 1;
        close(price(in,c).theo,100000,"One bp of paid funding cancels one bp of exit basis");

        close(projected_hourly_funding_bps(0.05,5,30),0,"No payment before next funding boundary");
        close(projected_hourly_funding_bps(0.05,30,30),0.05,"Include funding on horizon end");
        close(projected_hourly_funding_bps(0.05,90,30),0.10,"Count both future hourly boundaries");
        close(projected_hourly_funding_bps(-0.05,60,60),-0.05,"Negative rates are signed credits");

        in.expected_funding_paid_bps = 0; in.expected_exit_basis_bps = 0;
        c.round_trip_hedge_cost_bps = c.round_trip_profit_bps = 0;
        q = price(in,c);
        close(q.theo,100000,"Fees do not change theo");
        close(q.ask-q.bid,20,"Two-bps fee-only budget sets two-bps total quote spread");
        c.round_trip_fee_bps = 6;
        const auto wider = price(in,c);
        close(wider.theo,q.theo,"Higher fees affect quote width only");
        close(wider.ask-wider.bid,60,"Six-bps total fee budget does not become 24 bps");

        c = Config{}; in.inventory_btc = c.max_inventory_btc;
        c.round_trip_fee_bps = 2; c.round_trip_hedge_cost_bps = 2;
        q = price(in,c);
        close(q.bid_size_btc,0,"At long limit, disable buys that increase inventory");
        close(q.ask_size_btc,0.01,"At long limit, allow sells that reduce inventory");
        in.inventory_btc = -c.max_inventory_btc;
        q = price(in,c);
        close(q.ask_size_btc,0,"At short limit, disable sells that increase short inventory");
        close(q.bid_size_btc,0.01,"At short limit, allow buys that reduce inventory");
        in.inventory_btc = 0.09565;
        q = price(in,c);
        close(q.bid_size_btc,0.0043,"Round remaining inventory capacity down to size increment");
        check(in.inventory_btc+q.bid_size_btc <= c.max_inventory_btc,"Size rounding cannot exceed limit");

        in.inventory_btc = 0; in.expected_exit_basis_bps = 10;
        in.perp_bid = 100000; in.perp_ask = 100001;
        q = price(in,c);
        check(q.bid < in.perp_ask && q.ask > in.perp_bid,"Post-only bid guard must not cross best ask");
        in.expected_exit_basis_bps = -10;
        q = price(in,c);
        check(q.bid < in.perp_ask && q.ask > in.perp_bid,"Post-only ask guard must not cross best bid");

        in.perp_bid = in.perp_ask = 0;
        in.expected_exit_basis_bps = 0; in.spot = 100;
        q = price(in,c);
        close(q.bid_size_btc,0,"Disable sizes below configured minimum notional");
        close(q.ask_size_btc,0,"Minimum notional applies to both sides");
        in.spot = 100000.03;
        q = price(in,c);
        check(q.bid <= q.reservation_price-25 && q.ask >= q.reservation_price+25,
              "Tick rounding widens quotes rather than erasing the cost budget");

        bool rejected = false;
        in.spot = std::numeric_limits<double>::quiet_NaN();
        try { (void)price(in,c); } catch (const std::runtime_error&) { rejected = true; }
        check(rejected,"Reject non-finite market input");
        rejected = false;
        try { (void)projected_hourly_funding_bps(1,5,0); }
        catch (const std::runtime_error&) { rejected = true; }
        check(rejected,"Explicitly require time to NEXT hourly boundary");
        std::cout << "PASS: theo and funding signs/boundaries, aggregate fee width, inventory skew,\n"
            << "position limits, quote rounding, passive guards, minimum notional, and invalid inputs.\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
