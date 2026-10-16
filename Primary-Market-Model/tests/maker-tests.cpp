#include "../backtester/maker-backtest.h"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void check(bool good, const char* message) { if (!good) throw std::runtime_error(message); }
void close(double actual, double expected, const char* message) {
    if (std::abs(actual-expected) > 1e-8) {
        std::cerr << message << ": " << actual << " != " << expected << '\n';
        throw std::runtime_error(message);
    }
}
using namespace btc::maker_backtest;
constexpr Millis start = 8*3600000;
Event rate(Millis t, double bps, char kind = 'F') {
    Event e; e.time = t; e.kind = kind; e.price = 100000; e.funding_rate = bps/10000; return e;
}
Event spot(Millis t, double price = 100000) { Event e; e.time = t; e.kind = 'S'; e.price = price; return e; }
Event book(Millis t, double bid = 99900, double ask = 100100) {
    Event e; e.time = t; e.kind = 'Q'; e.bid = bid; e.ask = ask; return e;
}
Event trade(Millis t, double price, bool buyer_maker, double quantity = 0.01) {
    Event e; e.time = t; e.kind = 'T'; e.price = price; e.quantity = quantity; e.buyer_maker = buyer_maker; return e;
}
void seed(Engine& engine, double bps = 0.5) {
    engine.on_event(rate(start,bps),false);
    engine.on_event(spot(start),false);
    engine.on_event(book(start));
}
btc::maker::Config cost_config() {
    btc::maker::Config config;
    config.rule = btc::maker::QuoteRule::CostBudget;
    config.risk_buffer_bps = config.profit_bps = config.skew_bps_per_contract = 1;
    return config;
}
}
int main() {
    try {
        btc::maker::Inputs in;
        in.spot = 100000; in.perp_bid = 99900; in.perp_ask = 100100;
        auto c = cost_config();
        check(!btc::maker::price(in,c).enabled,"Unknown funding must not be treated as cheap funding");
        in.funding_ready = true; in.funding_bps = 0.5;
        auto q = btc::maker::price(in,c);
        check(q.enabled && q.bid < in.perp_ask && q.ask > in.perp_bid,"Low funding enables passive quotes");
        check(q.minimum_spread_bps >= 5 && q.expected_net_bps >= c.profit_bps,"Both maker fees plus risk and profit are covered");
        close(q.cycle_fees_bps,(q.ask+q.bid)*0.0002/in.spot*10000,"Charge two percentage fees at the actual limit prices");
        close(q.funding_reserve_bps,0,"No funding settlement within five minutes");
        close(q.lower_price_bound,99950,"Lower price bound is spot minus five bps");
        close(q.upper_price_bound,100050,"Upper price bound is spot plus five bps");
        for (double basis : {-5.01,5.01}) {
            auto outside = in;
            outside.perp_bid = in.spot*(1+basis/10000)-0.1;
            outside.perp_ask = outside.perp_bid+0.2;
            check(btc::maker::price(outside,c).reason == "BASIS_OUT_OF_RANGE","Both signs outside basis band disable quotes despite low funding");
        }
        for (double basis : {-5.0,5.0}) {
            auto edge = in;
            edge.perp_bid = in.spot*(1+basis/10000)-0.1;
            edge.perp_ask = edge.perp_bid+0.2;
            const auto bounded = btc::maker::price(edge,c);
            check(bounded.enabled,"Inclusive basis boundaries allow profitable passive quotes");
            check(bounded.bid >= bounded.lower_price_bound && bounded.ask <= bounded.upper_price_bound,
                  "Quote endpoints stay within spot basis band");
        }
        auto narrow = c;
        narrow.min_basis_bps = -2; narrow.max_basis_bps = 2;
        check(!btc::maker::price(in,narrow).enabled,"Skip if band cannot cover fees, risk and target profit");
        auto skewed = in; skewed.inventory_contracts = 5;
        const auto clipped = btc::maker::price(skewed,c);
        check(!clipped.enabled && clipped.reason == "INSUFFICIENT_EDGE","Clipping skewed quotes must not silently erase cost budget");
        c.fee_usd_per_contract = 0.15;
        const auto flat = btc::maker::price(in,c);
        check(flat.minimum_spread_bps > q.minimum_spread_bps+2.9,"Both flat per-contract fees widen the lower bound");
        c.fee_usd_per_contract = 0;
        in.minutes_to_funding = 2;
        q = btc::maker::price(in,c);
        close(q.funding_reserve_bps,0.5,"Reserve the full next funding payment across its boundary");
        in.funding_bps = -0.5;
        close(btc::maker::price(in,c).funding_reserve_bps,0.5,"Negative funding also costs the short inventory side");
        c.horizon_minutes = 482;
        close(btc::maker::price(in,c).funding_reserve_bps,1,"Count two eight-hour boundaries, not hourly payments");
        c.horizon_minutes = 5;
        for (double interval : {60.0,240.0}) {
            c.funding_interval_minutes = interval;
            c.horizon_minutes = interval+2;
            close(btc::maker::price(in,c).funding_reserve_bps,1,"Honor one-hour and four-hour archived intervals");
        }
        c.funding_interval_minutes = 480; c.horizon_minutes = 5;
        for (double funding : {-1.01,1.01}) {
            in.funding_bps = funding;
            check(btc::maker::price(in,c).reason == "HIGH_FUNDING","Both high funding signs disable quoting");
        }
        in.funding_bps = 1;
        check(btc::maker::price(in,c).enabled,"Funding cutoff is inclusive");
        c.max_spread_bps = 1;
        check(btc::maker::price(in,c).reason == "SPREAD_TOO_WIDE","Skip when the cost lower bound exceeds allowed width");
        c = cost_config(); c.maker_fee_bps = c.risk_buffer_bps = c.profit_bps = 0;
        in.funding_bps = 0;
        q = btc::maker::price(in,c);
        check(q.ask > q.bid && q.minimum_spread_bps > 0,"Zero-cost spread still spans at least one tick");
        bool rejected = false;
        c.maker_fee_bps = std::numeric_limits<double>::quiet_NaN();
        try { (void)btc::maker::price(in,c); } catch (const std::runtime_error&) { rejected = true; }
        check(rejected,"Non-finite fees rejected");

        Config replay_config;
        replay_config.pricing = cost_config();
        replay_config.latency_ms = 50; replay_config.reprice_ms = 0;
        replay_config.pricing.skew_bps_per_contract = 0;
        replay_config.participation = 1;
        Engine engine(replay_config);
        seed(engine);
        const auto initial = engine.decisions().back().quotes;
        engine.on_event(trade(start+20,initial.bid-0.1,true));
        engine.on_event(trade(start+50,initial.bid-0.1,true));
        check(engine.fills().empty(),"Cannot fill before or at the quote arrival timestamp");
        engine.on_event(trade(start+60,initial.bid,true));
        check(engine.fills().empty(),"Touch does not fill in conservative mode");
        engine.on_event(trade(start+61,initial.bid-0.1,false));
        check(engine.fills().empty(),"Wrong aggressor direction cannot fill the bid");
        engine.on_event(trade(start+62,initial.bid-0.1,true,0.004));
        check(engine.fills().size() == 1,"Eligible trade partially fills a resting order");
        close(engine.fills().back().btc,0.004,"Fill cannot exceed observed volume");
        engine.on_event(trade(start+63,initial.bid-0.1,true,0.006));
        close(engine.totals().inventory,0.01,"Partial fills sum to the order quantity");
        engine.on_event(trade(start+64,initial.ask+0.1,false,0.01));
        close(engine.totals().inventory,0,"Both maker sides complete the cycle");
        close(engine.totals().fees,0.01*(initial.bid+initial.ask)*0.0002,"Fees on all partial buys and the sell");
        close(engine.totals().net,0.01*(initial.ask-initial.bid)-engine.totals().fees,"Closed cycle reconciles to cash flows");
        check(engine.totals().net > 0,"Static completed cycle earns profit after both fees");
        engine.finish();
        check(engine.fills().size() == 3,"Finish invents no liquidation fill");

        Engine post_only(replay_config);
        seed(post_only);
        post_only.on_event(book(start+20,initial.bid-10,initial.bid),false);
        post_only.on_event(trade(start+60,initial.bid-0.1,true));
        check(post_only.fills().empty() && post_only.post_only_rejections() == 1,"Post-only checked against arrival book");

        auto capped = replay_config; capped.max_inventory_btc = 0.005;
        Engine cap(capped); seed(cap);
        cap.on_event(trade(start+60,initial.bid-0.1,true,1));
        cap.on_event(trade(start+120,initial.bid-0.1,true,1));
        close(cap.totals().inventory,0.005,"Fills and pending quotes respect inventory cap");

        auto participant = replay_config; participant.participation = 0.1;
        Engine participation(participant); seed(participation);
        participation.on_event(trade(start+60,initial.bid-0.1,true,0.04));
        close(participation.totals().inventory,0.004,"Trade participation caps each partial fill");

        Engine high(replay_config); seed(high,-2);
        high.on_event(trade(start+100,99000,true,1));
        check(high.fills().empty(),"High-funding window opens no positions");
        Engine outside(replay_config); seed(outside);
        outside.on_event(book(start+60,100060,100070));
        check(outside.decisions().back().quotes.reason == "BASIS_OUT_OF_RANGE","Basis guard requests withdrawal of live quotes");
        outside.on_event(trade(start+120,initial.bid-0.1,true,1));
        check(outside.fills().empty(),"Outside-band quotes are canceled after exchange latency");
        Engine stale(replay_config); seed(stale);
        stale.on_event(book(start+100000));
        check(stale.decisions().back().quotes.reason == "STALE_MARKET","Stale spot reference withdraws quotes");

        Engine funding(replay_config); seed(funding);
        funding.on_event(trade(start+60,initial.bid-0.1,true));
        funding.on_event(rate(start+8*3600000,-0.5),false);
        close(funding.totals().funding,0.01*100000*0.00005,"Long receives actual negative Binance settlement");
        check(funding.totals().inventory == 0.01,"Funding does not silently close inventory");

        // The new default follows exactly spot +/- the per-fill maker fee.
        btc::maker::Config fee_only;
        auto fee_in = in;
        fee_in.spot = 100000; fee_in.perp_bid = 99990; fee_in.perp_ask = 100010;
        fee_in.funding_ready = false; fee_in.inventory_contracts = 5;
        const auto fixed = btc::maker::price(fee_in,fee_only);
        check(fixed.enabled && fixed.bid_enabled && fixed.ask_enabled,"Fee quotes need only the basis gate, not a settled funding forecast");
        close(fixed.bid,99980,"Fee-only bid = spot minus two bps, without inventory skew");
        close(fixed.ask,100020,"Fee-only ask = spot plus two bps");
        close(fixed.quoted_spread_bps,4,"No added risk/profit/funding spread margin");
        fee_in.perp_bid = 100030; fee_in.perp_ask = 100030.1;
        const auto one_side = btc::maker::price(fee_in,fee_only);
        check(one_side.enabled && one_side.bid_enabled && !one_side.ask_enabled,"Reject a marketable side without moving its spot-fee price");
        close(one_side.ask,100020,"Post-only does not widen the fee-only ask");
        fee_only.maker_fee_bps = 6;
        check(btc::maker::price(fee_in,fee_only).reason == "FEE_QUOTES_OUT_OF_BAND","Do not quote if either fee endpoint leaves the five-bp band");

        Config instant;
        instant.participation = 1;
        check(instant.latency_ms == 0 && instant.reprice_ms == 0,"Default replay has zero latency and reprice delay");
        Engine immediate(instant); seed(immediate,10); // Old high-rate gate must not override the user basis rule.
        const auto instant_quotes = immediate.decisions().back().quotes;
        immediate.on_event(trade(start,instant_quotes.bid-0.1,true));
        check(immediate.fills().empty(),"Zero latency cannot fill from an already-observed timestamp");
        immediate.on_event(trade(start+1,instant_quotes.bid-0.1,true));
        check(immediate.fills().size() == 1,"Zero-latency quote is live for the next trade with no additional event delay");
        immediate.on_event(book(start+2,100060,100070));
        immediate.on_event(trade(start+3,instant_quotes.bid-0.1,true));
        check(immediate.fills().size() == 1,"Zero-latency cancellation immediately withdraws outside-band quotes");
        Engine instant_batch(instant); seed(instant_batch);
        instant_batch.on_event(book(start+1,100060,100070),false);
        instant_batch.on_event(trade(start+1,99970,true));
        check(instant_batch.fills().empty(),"Zero latency respects a basis breach known earlier in the same timestamp");
        Engine instant_spot(instant); seed(instant_spot);
        instant_spot.on_event(spot(start+1,99990),false);
        instant_spot.on_event(trade(start+2,99975,true));
        check(instant_spot.fills().empty(),"A known spot update replaces the old bid immediately, including within an event batch");
        Engine midpoint(instant);
        Event spot_book; spot_book.time = start; spot_book.kind = 'S'; spot_book.bid = 99999; spot_book.ask = 100001;
        midpoint.on_event(spot_book,false);
        midpoint.on_event(book(start));
        close(midpoint.decisions().back().quotes.reference,100000,"Observed spot bid/ask is converted to its midpoint");
        check(midpoint.spot_mid_events() == 1 && midpoint.spot_proxy_events() == 0,"Distinguish true midpoints from proxy data");
        Engine decimal_touch(instant);
        decimal_touch.on_event(spot(start,30161.71),false);
        decimal_touch.on_event(book(start,30155.6,30155.7));
        decimal_touch.on_event(trade(start+1,30155.6,true));
        check(decimal_touch.fills().empty(),"Decimal tick rounding cannot turn a touch into a through fill");
        decimal_touch.on_event(trade(start+2,30155.5,true));
        check(decimal_touch.fills().size() == 1,"A full decimal tick through still fills");
        fee_in.spot = 30161.71; fee_in.perp_bid = 30167.8; fee_in.perp_ask = 30167.9;
        fee_only.maker_fee_bps = 2;
        check(!btc::maker::price(fee_in,fee_only).ask_enabled,"Decimal rounding cannot pass a marketable ask as post-only");

        std::cout << "PASS: Binance maker fees, +/-5 bps basis gate and quote bounds, funding signs/cutoff/eight-hour boundaries,\n"
            << "latency, aggressor direction, through-only partial fills, participation, post-only arrival,\n"
            << "inventory caps, stale references, funding cash flows and net-PnL reconciliation.\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
