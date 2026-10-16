#include "../backtester/basis-maker.h"
#include <cmath>
#include <filesystem>
#include <fstream>
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
Event premium(Millis t,double value) { Event e; e.time=t; e.kind='P'; e.price=value; return e; }
void seed(Engine& e,Millis t=start) { e.on_event(funding(t)); e.on_event(spot(t)); e.on_event(book(t)); }
void mixed(Engine& e) {
    seed(e); e.on_event(trade(start+1,99940,true));
    e.on_event(book(start+2,99970,99980)); e.on_event(trade(start+3,99980,false));
}
void minutes(Engine& e,double value,int count=478) {
    for (int i=1;i<=count;++i) e.on_event(premium(start+i*60000,value));
}
}
int main() {
    try {
        Config legacy; legacy.exit_on_equilibrium=false; legacy.max_gross_perp_btc=.05;
        legacy.exit_before_funding=false; legacy.funding_cashflows=false;
        legacy.perp_maker_bps=2; legacy.perp_taker_bps=5; legacy.spot_taker_bps=10;
        Engine engine(legacy); seed(engine);
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

        Config guarded_config; guarded_config.exit_before_funding=false; guarded_config.funding_cashflows=false;
        Engine guarded(guarded_config); seed(guarded);
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
        Config exits; exits.exit_on_equilibrium=true; exits.exit_before_funding=false; exits.funding_cashflows=false; exits.perp_maker_bps=0; exits.perp_taker_bps=0; exits.spot_taker_bps=0;
        Engine convergence(exits); seed(convergence);
        convergence.on_event(trade(start+1,99940,true));
        convergence.on_event(trade(start+2,99940,true));
        check(convergence.pairs().size()==2,"Multiple pairs enter before any pair exits");
        close(convergence.totals().open_gross_perp_btc,.008,"Independent concurrent pair quantities accumulate");
        convergence.on_event(book(start+3,99950,99960));
        check(convergence.pairs()[0].closed && convergence.pairs()[1].closed,"Long pairs close when midpoint crosses -5 bps from below");
        check(convergence.fills().size()==8,"Each closed pair has four leg fills");
        close(convergence.fills()[4].price,99950,"Long perp exit sells executable bid");
        close(convergence.fills()[5].price,100005,"Short spot exit buys executable ask");
        check(convergence.fills()[4].liquidity=="TAKER" && convergence.fills()[4].phase=="EXIT","Perp exit is explicitly a taker fill");
        close(convergence.totals().perp_btc,0,"Closing pairs leaves no perp exposure");
        close(convergence.totals().spot_btc,0,"Closing pairs leaves no spot exposure");
        close(convergence.totals().realized_pnl,0,"Both crossed spreads included in realized round-trip PnL");
        convergence.on_event(trade(start+4,99960,false));
        check(convergence.pairs().size()==3,"New entry resumes after convergence");
        convergence.on_event(book(start+5,99939,99949));
        check(convergence.pairs()[2].closed,"Short pair closes when midpoint crosses from above");
        close(convergence.pairs()[2].exit_perp_price,99949,"Short perp exit buys ask");
        close(convergence.pairs()[2].exit_spot_price,99995,"Long spot exit sells bid");
        close(convergence.pairs()[2].realized_pnl,.004,"Short round-trip signed cash is correct");
        close(convergence.totals().net(),convergence.totals().realized_pnl,"Flat ledger equals realized PnL");

        Config recycle=exits; recycle.max_gross_perp_btc=.005;
        Engine recycling(recycle); seed(recycling);
        recycling.on_event(trade(start+1,99940,true,1));
        recycling.on_event(trade(start+2,99940,true,1));
        close(recycling.totals().open_gross_perp_btc,.005,"Optional cap constrains concurrently open BTC");
        recycling.on_event(book(start+3,99950,99960));
        recycling.on_event(trade(start+4,99960,false,1));
        close(recycling.totals().gross_perp_btc,.010,"Convergence exits recycle capacity for future entries");
        close(recycling.totals().open_gross_perp_btc,.005,"Recycled cap remains bounded");

        Engine unlimited(exits); seed(unlimited);
        for (int i=1;i<=20;++i) unlimited.on_event(trade(start+i,99940,true,1));
        check(unlimited.pairs().size()==20,"No one-pair restriction or cumulative entry cap by default");
        close(unlimited.totals().open_gross_perp_btc,.2,"Unlimited experiment exceeds old 0.05 BTC cap");
        unlimited.finish();
        check(!unlimited.pairs()[0].closed,"Replay end does not close unconverged pairs");

        Config cycle=exits; cycle.perp_maker_bps=.5; cycle.perp_taker_bps=.5; cycle.spot_taker_bps=.5;
        Engine fees_exit(cycle); seed(fees_exit);
        close(fees_exit.config().perp_maker_bps+fees_exit.config().perp_taker_bps+2*fees_exit.config().spot_taker_bps,
              2,"Default maker plus three taker rates total 2 bps per cycle");
        fees_exit.on_event(trade(start+1,99940,true));
        close(fees_exit.totals().fees,.004*(99940+99995)*.00005,
              "Open pair pays only its two entry fees; future exit fees are not charged early");
        fees_exit.on_event(book(start+2,99950,99960));
        const auto& closed=fees_exit.pairs()[0];
        const double expected_fees=.004*(99940+99995+99950+100005)*.00005;
        close(fees_exit.totals().fees,expected_fees,"All four entry/exit fees use actual filled notionals");
        close(closed.realized_pnl,-expected_fees,"Convergence can realize a loss after costs");

        Engine stale(exits); seed(stale);
        stale.on_event(trade(start+1,99940,true));
        stale.on_event(book(start+90001,99950,99960));
        check(!stale.pairs()[0].closed,"Stale hedge reference prevents fabricated exit");
        stale.on_event(spot(start+90002));
        check(stale.pairs()[0].closed,"Fresh spot reference enables convergence exit");

        Engine blackout(exits); seed(blackout);
        blackout.on_event(spot(due-61000)); blackout.on_event(book(due-61000));
        blackout.on_event(trade(due-60999,99940,true));
        blackout.on_event(spot(due-60000));
        blackout.on_event(book(due-60000,99950,99960));
        check(blackout.pairs()[0].closed,"Pre-funding blackout blocks entries, not risk-reducing exits");

        const auto directory=std::filesystem::temp_directory_path()/"btc-basis-continuation-test";
        std::filesystem::create_directories(directory);
        const std::string header="time_ms,kind,price,quantity,bid,ask,buyer_maker,funding_rate,interval_hours\n";
        {
            std::ofstream a(directory/"a.csv"),b(directory/"b.csv");
            a << header << "28800000,F,100000,,,,,0,8\n28800000,S,100000,,,,,,\n"
              << "28800000,Q,,,99940,99950,,,\n28800001,T,99940,0.04,,,true,,\n";
            b << header << "0,R,,,,,,0,8\n28800002,Q,,,99950,99960,,,\n"
              << "28800003,T,99960,0.04,,,false,,\n";
        }
        Engine continued(exits);
        replay_files(continued,{(directory/"a.csv").string(),(directory/"b.csv").string()});
        check(continued.pairs().size()==2 && continued.pairs()[0].closed && !continued.pairs()[1].closed,
              "Multiple event files preserve held pairs, funding schedule and quote state across the boundary");
        std::filesystem::remove_all(directory);

        Config holding;
        check(!holding.exit_on_equilibrium && holding.exit_before_funding && holding.max_gross_perp_btc==0,
              "Default policy holds without a cap and exits only before funding");
        close(holding.perp_maker_bps+holding.perp_taker_bps+2*holding.spot_taker_bps,0,"Default execution fees are zero");
        Engine high; mixed(high);
        check(high.pairs().size()==2 && !high.pairs()[0].closed && !high.pairs()[1].closed,
              "Opposite-direction pairs coexist and convergence does not close them");
        minutes(high,.0007);
        high.on_event(spot(due-61000)); high.on_event(book(due-61000,99970,99980));
        check(!high.pairs()[0].closed,"Linear funding region alone cannot exit before the final minute");
        high.on_event(premium(due-60000,.0007));
        check(high.premium_ready(),"All completed premium minutes are available causally");
        close(high.average_premium_bps(),7,"Weighted premium average uses bps");
        close(high.estimated_funding_bps(),2,"Above +6 bps premium, funding scales linearly as P-5");
        check(high.pairs()[0].closed && !high.pairs()[1].closed,"Positive funding closes long-perp payers and retains short receivers");
        check(high.pairs()[0].exit_reason=="PRE_FUNDING_LINEAR_REGION","No convergence exit is hidden in the funding-only policy");
        high.on_event(funding(due,.0002));
        close(high.totals().funding_pnl,.08,"Retained short receives actual settlement funding");
        close(high.totals().realized_pnl,high.pairs()[0].realized_pnl+.08,"Funding receipts are realized immediately while a pair stays open");
        check(!high.premium_ready(),"Settlement resets the premium window rather than reusing future-period data");

        Engine negative; mixed(negative); minutes(negative,-.0008);
        negative.on_event(spot(due-61000)); negative.on_event(book(due-61000,99970,99980));
        negative.on_event(premium(due-60000,-.0008));
        check(!negative.pairs()[0].closed && negative.pairs()[1].closed,"Negative funding closes short payers while retaining long receivers");
        negative.on_event(funding(due,-.0003));
        close(negative.totals().funding_pnl,.12,"Retained long receives negative-rate settlement");

        Engine flat; seed(flat); flat.on_event(trade(start+1,99940,true)); minutes(flat,0);
        flat.on_event(spot(due-61000)); flat.on_event(book(due-61000)); flat.on_event(premium(due-60000,0));
        close(flat.estimated_funding_bps(),1,"Binance flat funding region is +1 bp per eight hours, not zero");
        check(!flat.pairs()[0].closed,"Flat-region positions are held across funding");
        flat.on_event(funding(due,.0001));
        close(flat.totals().funding_pnl,-.04,"Low funding still charges held long positions");
        close(flat.totals().realized_pnl,-.04,"Held-position funding payments are realized cash, not unrealized price PnL");
        flat.finish(); check(!flat.pairs()[0].closed,"Replay end leaves held lots open");

        Engine lower; mixed(lower); minutes(lower,-.00045);
        lower.on_event(spot(due-61000)); lower.on_event(book(due-61000,99970,99980));
        lower.on_event(premium(due-60000,-.00045));
        close(lower.estimated_funding_bps(),.5,"Lower linear region can still have a positive funding rate");
        check(lower.pairs()[0].closed && !lower.pairs()[1].closed,"Payer direction follows estimated funding sign, not premium sign");

        Engine gap; mixed(gap);
        for (int i=1;i<=478;++i) if (i!=200) gap.on_event(premium(start+i*60000,.0007));
        gap.on_event(spot(due-61000)); gap.on_event(book(due-61000,99970,99980));
        gap.on_event(premium(due-60000,.0007));
        check(!gap.premium_ready() && !gap.pairs()[0].closed,"Incomplete premium history cannot fabricate a funding-exit forecast");

        Engine jitter;
        jitter.on_event(funding(start+1)); jitter.on_event(spot(start+1)); jitter.on_event(book(start+1));
        jitter.on_event(trade(start+2,99940,true)); minutes(jitter,.0007);
        jitter.on_event(spot(due-61000)); jitter.on_event(book(due-61000));
        jitter.on_event(premium(due-60000,.0007));
        check(jitter.premium_ready() && jitter.pairs()[0].closed,"Millisecond publication jitter does not invalidate premium coverage");
        std::cout << "PASS: -5 bps equilibrium, passive perp entries, immediate equal-BTC spot hedges, both fee signs,\n"
            << "concurrent pairs, two-sided convergence exits, all four fees, optional recycled cap, legacy no-exit replay,\n"
            << "zero funding, pre-funding cancellations, partial fills, proxy/real spot spreads and realized/unrealized PnL.\n";
        return 0;
    } catch(const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
