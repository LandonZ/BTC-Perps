#include "../paper/paper-engine.h"
#include <cmath>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
using namespace btc::paper;
void check(bool good,const char* message) { if (!good) throw std::runtime_error(message); }
void close(double actual,double expected,const char* message) {
    if (std::abs(actual-expected) > 1e-9) {
        std::cerr << message << ": actual=" << actual << ", expected=" << expected << '\n';
        throw std::runtime_error(message);
    }
}
Event quote(Millis t,Leg leg,double bid,double ask) { return Event{t,'Q',leg,bid,1,ask,1,0,0}; }
Event trade(Millis t,double price) { return Event{t,'T',Perp,0,0,0,0,price,1}; }
const Millis t0 = 1760000400000; // Exact UTC hour.
}

int main() {
    try {
        // Reader: header and other products skipped, malformed rows counted.
        std::istringstream csv(
            "recv_ms,exch_ms,kind,product,bid,bid_qty,ask,ask_qty,price,size,side\n"
            "1000,990,Q,BTC-USD,100.0,1.0,101.0,3.0,,,\n"
            "1001,995,T,BIP-20DEC30-CDE,,,,,100005,2,BUY\n"
            "1002,995,Q,ETH-USD,1,1,2,2,,,\n"
            "1003,995,Q,BTC-USD,101,1,100,1,,,\n");
        EventReader reader(csv,"BIP-20DEC30-CDE","BTC-USD");
        Event e;
        check(reader.next(e) && e.leg == Spot && e.kind == 'Q' && e.ask == 101 && e.time == 1000,"Spot quote row");
        check(reader.next(e) && e.leg == Perp && e.kind == 'T' && e.price == 100005 && e.size == 2,"Perp trade row");
        check(!reader.next(e) && reader.skipped() == 1,"Crossed quote is rejected, other products ignored");

        // Taker: the signal fires on the current book, but the order fills at the book
        // in force when it arrives `latency` later. Fees on both legs.
        Config c;
        c.latency_ms = 50; c.perp_taker_bps = 2; c.spot_taker_bps = 1; c.perp_fee_usd_per_contract = 0.05;
        Engine taker(c);
        taker.on_event(quote(t0,Spot,100000,100010));
        taker.on_event(quote(t0+10,Perp,100100,100105));   // sell basis = 100100/100010 - 1 = 9.0 bp > 5
        check(taker.fills().empty(),"Nothing fills before the latency elapses");
        taker.on_event(quote(t0+40,Perp,100090,100095));   // book moves before our order arrives
        taker.on_event(quote(t0+70,Spot,100000,100010));   // first event after arrival at t0+60
        check(taker.fills().size() == 2 && taker.position_contracts() == -1,"Short perp / long spot entered");
        check(taker.fills()[0].time == t0+60 && taker.fills()[0].price == 100090,"Perp sold at the bid in force on arrival");
        check(taker.fills()[1].price == 100010,"Spot bought at the ask");
        const double entry_fees = 0.01*100090*2e-4 + 0.05 + 0.01*100010*1e-4;
        // Exit: perp falls so the round trip is profitable after fees.
        taker.on_event(quote(t0+1000,Perp,99990,99995));
        taker.on_event(quote(t0+1100,Spot,100000,100010)); // exit order arrives (t0+1050 due)
        check(taker.trips().size() == 1 && taker.position_contracts() == 0,"Profit exit completed");
        const auto& trip = taker.trips()[0];
        check(trip.exit_reason == "PROFIT" && trip.direction == -1,"Exit reason and direction");
        close(trip.exit_perp,99995,"Perp bought back at the ask");
        close(trip.exit_spot,100000,"Spot sold at the bid");
        close(trip.price_pnl,-0.01*((99995-100090)-(100000-100010)),"Short-basis price PnL");
        close(trip.fees,entry_fees + 0.01*99995*2e-4 + 0.05 + 0.01*100000*1e-4,"Round-trip fees");
        close(trip.net_pnl,trip.price_pnl-trip.fees,"Net = price - fees (no funding hour crossed)");
        close(trip.signal_basis_bps,(100100.0/100010-1)*1e4,"Signal basis recorded");
        close(trip.entry_basis_bps,(100090.0/100010-1)*1e4,"Fill basis shows latency slippage");
        close(taker.totals().net(),trip.net_pnl,"Totals match the closed trade");

        // Timeout and forced end with zero latency.
        Config z; z.latency_ms = 0; z.exit_on_profit = false; z.max_hold_ms = 60000;
        Engine timeout(z);
        timeout.on_event(quote(t0,Spot,100000,100000),false);
        timeout.on_event(quote(t0,Perp,99900,99900));            // -10 bp: buy basis immediately
        check(timeout.position_contracts() == 1,"Zero latency fills at the same timestamp");
        timeout.on_event(quote(t0+60000,Perp,99901,99901));
        check(timeout.trips().size() == 1 && timeout.trips()[0].exit_reason == "TIMEOUT","Max hold forces the exit");
        timeout.on_event(quote(t0+120000,Perp,99800,99800));
        timeout.finish();
        check(timeout.trips().size() == 2 && timeout.trips()[1].forced_end,"Open position closed at the end");

        // Deferred decisions: a half-updated candle (new perp, old spot) must not trade.
        Engine batch(z);
        batch.on_event(quote(t0,Spot,100000,100000),false);
        batch.on_event(quote(t0,Perp,100000,100000));
        batch.on_event(quote(t0+60000,Perp,100100,100100),false); // 10 bp vs the OLD spot
        batch.on_event(quote(t0+60000,Spot,100100,100100));        // spot catches up: 0 bp
        check(batch.fills().empty(),"No trade on a half-updated timestamp");

        // Funding: a position held across the hour pays the BIP rate on the perp leg.
        Config f; f.latency_ms = 0; f.exit_on_profit = false; f.max_hold_ms = 2*3600000; f.threshold_bps = 50;
        Engine funding(f);
        funding.on_event(quote(t0,Spot,100000,100000),false);
        funding.on_event(quote(t0,Perp,99000,99000));             // -100 bp: long perp
        check(funding.position_contracts() == 1,"Long basis for the funding test");
        funding.on_event(quote(t0+3600000,Spot,100000,100000));
        // The whole hour had a -100 bp gap: premium = -100/24 bp, rate = 0.75 * premium.
        const double rate = 0.75*(99000.0-100000)/100000/24*1e4;
        close(funding.totals().funding_pnl,-btc::bip::payment_usd(1,99000,rate),"Long receives negative funding");
        check(funding.totals().funding_pnl > 0,"Negative funding credits the long perp");

        // Maker: quotes around spot theo, on the $5 grid, live only after the latency,
        // and filled only by trades printing through them.
        Config m; m.mode = Mode::Maker; m.latency_ms = 100; m.threshold_bps = 5; m.skew_bps_per_contract = 1;
        m.perp_maker_bps = 1;
        Engine maker(m);
        maker.on_event(quote(t0,Spot,99999,100001),false);
        maker.on_event(quote(t0,Perp,99900,100100));                // bid 99950, ask 100050
        maker.on_event(trade(t0+50,99940));
        check(maker.fills().empty(),"Quote not live before the latency");
        maker.on_event(trade(t0+150,99950));
        check(maker.fills().empty(),"A print AT our bid does not fill by default");
        maker.on_event(trade(t0+160,99945));
        check(maker.fills().size() == 1 && maker.position_contracts() == 1,"A print through our bid fills us");
        close(maker.fills()[0].price,99950,"Filled at our quote, not the print");
        close(maker.totals().fees,0.01*99950*1e-4,"Maker fee");
        close(maker.totals().price_pnl,-0.01*99950+0.01*100000,"Inventory marked at the perp mid");
        maker.on_event(trade(t0+170,99900));
        check(maker.fills().size() == 1,"Filled quote is gone until the re-quote arrives");
        maker.on_event(trade(t0+300,100060));                        // skewed ask: ceil(100000*(1-1e-4)*(1+5e-4)) = 100040
        check(maker.fills().size() == 2 && maker.position_contracts() == 0,"Ask filled after the re-quote");
        close(maker.fills()[1].price,100040,"Inventory skew lowers the ask from 100050 to 100040");

        Config touch = m; touch.fill_at_touch = true; touch.latency_ms = 0;
        Engine toucher(touch);
        toucher.on_event(quote(t0,Spot,99999,100001),false);
        toucher.on_event(quote(t0,Perp,99900,100100));
        toucher.on_event(trade(t0+1,99950));
        check(toucher.fills().size() == 1,"--fill-at-touch fills on a print at our price");

        std::cout << "PASS: event CSV parsing, latency-delayed taker fills at the arrival book, fees,\n"
            << "profit/timeout/forced exits, same-timestamp batching, BIP funding on held positions,\n"
            << "maker quote grid, through-only fills, touch fills and inventory skew.\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
