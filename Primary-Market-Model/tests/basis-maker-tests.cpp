#include "../backtester/basis-maker.h"
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
using namespace btc::basis_maker;
void check(bool ok,const char* message) { if (!ok) throw std::runtime_error(message); }
void close(double a,double b,const char* message) {
    if (std::abs(a-b)>1e-8) { std::cerr << a << " != " << b << '\n'; throw std::runtime_error(message); }
}
constexpr Millis start = 8*3600000;
Event spot(Millis t,double price = 100000) { Event e; e.time=t; e.kind='S'; e.price=price; return e; }
Event book(Millis t,double bid=99940,double ask=99950) { Event e; e.time=t; e.kind='Q'; e.bid=bid; e.ask=ask; return e; }
Event trade(Millis t,double price,bool buyer_maker,double size=.04) {
    Event e; e.time=t; e.kind='T'; e.price=price; e.buyer_maker=buyer_maker; e.quantity=size; return e;
}
Event funding(Millis t,double rate=0.01) { Event e; e.time=t; e.kind='F'; e.interval_hours=8; e.price=100000; e.funding_rate=rate; return e; }
void seed(Engine& e,Millis t=start) { e.on_event(funding(t)); e.on_event(spot(t)); e.on_event(book(t)); }
}
int main() {
    try {
        Engine engine; seed(engine);
        check(engine.decisions().back().side==1,"Below -5 bps join the best perp bid");
        close(engine.decisions().back().price,99940,"Quote at bottom of actual perp spread");
        check(engine.fills().empty(),"Quoting alone creates neither perp nor spot fill");
        engine.on_event(trade(start,99940,true));
        check(engine.fills().empty(),"No fill on the quote creation timestamp");
        engine.on_event(trade(start+1,99940,false));
        check(engine.fills().empty(),"Wrong aggressor cannot fill a bid");
        engine.on_event(trade(start+2,99940,true));
        check(engine.pairs().size()==1 && engine.fills().size()==2,"Perp partial fill immediately creates exactly one spot hedge");
        const auto& p = engine.fills()[0]; const auto& s = engine.fills()[1];
        check(p.leg=="PERP" && p.liquidity=="MAKER" && p.side==1,"Perp maker buy");
        check(s.leg=="SPOT" && s.liquidity=="TAKER" && s.side==-1 && p.time==s.time,"Spot short crosses immediately");
        close(p.btc,.004,"Participation creates partial fills"); close(p.btc,s.btc,"Equal BTC quantities");
        close(s.price,99995,"Spot short sells the bid, including proxy spread");
        close(p.fee,.004*99940*.0002,"Perp entry maker fee"); close(s.fee,.004*99995*.001,"Spot entry taker fee");
        close(engine.totals().perp_btc+engine.totals().spot_btc,0,"Zero immediate net BTC exposure");
        check(std::abs(p.notional-s.notional)>0,"Equal BTC at different prices does not mean equal dollars");
        engine.on_event(book(start+3,99950,99960));
        check(engine.fills().size()==2,"Crossing equilibrium does not execute an exit");
        check(engine.decisions().back().side==-1,"Above -5 bps join perp ask");
        engine.on_event(trade(start+4,99960,false,.02));
        check(engine.pairs().size()==2,"Opposite signal creates another entry pair");
        check(engine.fills()[2].side==-1 && engine.fills()[3].side==1,"Perp maker sale is hedged by spot taker buy");
        close(engine.fills()[3].price,100005,"Spot long crosses the ask");
        close(engine.totals().gross_perp_btc,.006,"Gross capacity tracks both entry directions");
        close(engine.totals().perp_btc,.002,"Opposite entry offsets net exposure");
        engine.on_event(book(start+5,99949,99951));
        check(engine.decisions().back().side==0,"Exactly -5 bps quotes neither side");
        const auto n = engine.fills().size(); engine.finish();
        check(engine.fills().size()==n,"No automatic end-of-replay liquidation");

        Engine guarded; seed(guarded);
        const Millis due=start+8*3600000;
        guarded.on_event(spot(due-61000)); guarded.on_event(book(due-61000));
        guarded.on_event(trade(due-60000,99940,true));
        check(guarded.fills().empty(),"Cancel from the exact pre-funding cutoff, even if the next event is a trade");
        check(guarded.decisions().back().reason=="PRE_FUNDING_CANCEL","Funding cancellation is explicit");
        guarded.on_event(funding(due));
        guarded.on_event(spot(due)); guarded.on_event(book(due));
        guarded.on_event(trade(due+1,99940,true));
        check(!guarded.fills().empty(),"New entries resume when a new funding schedule is observed");
        guarded.on_event(funding(due+8*3600000,-.5));
        close(guarded.totals().funding_pnl,0,"User scenario forces funding to zero even for held pairs");
        check(!guarded.pairs().empty(),"Funding does not silently exit existing pairs");

        Config strict; strict.fill_at_touch=false;
        Engine through(strict); seed(through);
        through.on_event(trade(start+1,99940,true));
        check(through.fills().empty(),"Optional strict-through execution excludes touch prints");
        through.on_event(trade(start+2,99939.9,true));
        check(through.fills().size()==2,"Strict-through print hedges immediately");

        Config capped; capped.max_gross_perp_btc=.005;
        Engine cap(capped); seed(cap);
        cap.on_event(trade(start+1,99940,true,1));
        cap.on_event(trade(start+2,99939,true,1));
        close(cap.totals().gross_perp_btc,.005,"No-exit gross position cap cannot be exceeded");

        Engine actual_book;
        Event actual=spot(start); actual.bid=99998; actual.ask=100002;
        actual_book.on_event(actual); actual_book.on_event(book(start));
        actual_book.on_event(trade(start+1,99940,true));
        close(actual_book.fills()[1].price,99998,"Observed spot bid takes precedence over proxy spread");
        check(actual_book.spot_mid_events()==1 && actual_book.spot_proxy_events()==0,"Real spot books distinguished from proxies");

        const auto t=engine.totals();
        double cash=0,fees=0;
        for (const auto& fill:engine.fills()) { cash-=fill.side*fill.btc*fill.price; fees+=fill.fee; }
        close(t.price_pnl,cash+t.perp_btc*99950+t.spot_btc*100000,"Two-leg midpoint mark reconciles to signed cash flows");
        close(t.net(),t.price_pnl-fees,"Net includes fees on both entry legs and no invented exit costs");
        std::cout << "PASS: -5 bps equilibrium, passive perp entries, immediate equal-BTC spot hedges, both fee signs,\n"
            << "no exits, zero funding, pre-funding cancellations, partial fills, gross cap, proxy/real spot spreads and paired PnL.\n";
        return 0;
    } catch(const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
