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
void verify_side_attribution(const Engine& engine) {
    const auto actual = engine.side_totals();
    std::array<SideTotals,2> expected;
    for (const auto& p : engine.pairs()) {
        auto& side = expected[p.direction > 0 ? 0 : 1];
        ++side.entries; side.fees += p.fees+p.exit_fees; side.funding_pnl += p.funding_pnl;
        if (p.closed) { ++side.closed_pairs; side.realized_price_pnl += p.realized_pnl-p.funding_pnl; }
        else {
            side.open_btc += p.btc;
            side.unrealized_pnl += p.direction*p.btc*((engine.final_perp_mid()-p.perp_price)-
                                                     (engine.final_spot_mid()-p.spot_price))-p.fees;
        }
    }
    for (std::size_t i=0;i<expected.size();++i) {
        check(actual[i].entries==expected[i].entries && actual[i].closed_pairs==expected[i].closed_pairs,
              "Direction counts reconcile to individual hedged pairs");
        close(actual[i].open_btc,expected[i].open_btc,"Direction BTC reconciles to open pairs");
        close(actual[i].unrealized_pnl,expected[i].unrealized_pnl,"Direction unrealized excludes funding, includes entry fees and both legs");
        close(actual[i].realized_price_pnl,expected[i].realized_price_pnl,"Direction realized price PnL excludes previously paid funding");
        close(actual[i].funding_pnl,expected[i].funding_pnl,"Direction funding reconciles to pair cash flows");
        close(actual[i].fees,expected[i].fees,"Direction fee attribution uses all executed fills");
    }
    const auto total = engine.totals();
    close(actual[0].net()+actual[1].net(),total.net(),"Direction net PnLs sum to portfolio net");
    for (const auto& point : engine.equity()) {
        close(point.sides[0].net()+point.sides[1].net(),point.totals.net(),"Sampled direction PnL reconciles at every saved mark");
        close(point.sides[0].unrealized_pnl+point.sides[1].unrealized_pnl,
              point.totals.net()-point.totals.realized_pnl,"Sampled unrealized curves sum to portfolio unrealized");
    }
}
}
int main() {
    try {
        Config unbuffered; unbuffered.entry_offset_bps=0;
        Config legacy=unbuffered; legacy.exit_on_equilibrium=false; legacy.max_gross_perp_btc=.05;
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

        Config guarded_config=unbuffered; guarded_config.exit_before_funding=false; guarded_config.funding_cashflows=false;
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

        Config strict=unbuffered; strict.fill_at_touch=false;
        Engine through(strict); seed(through);
        through.on_event(trade(start+1,99940,true));
        check(through.fills().empty(),"Optional strict-through execution excludes touch prints");
        through.on_event(trade(start+2,99939.9,true));
        check(through.fills().size()==2,"Strict-through print hedges immediately");

        Config capped=unbuffered; capped.max_gross_perp_btc=.005;
        Engine cap(capped); seed(cap);
        cap.on_event(trade(start+1,99940,true,1));
        cap.on_event(trade(start+2,99939,true,1));
        close(cap.totals().gross_perp_btc,.005,"No-exit gross position cap cannot be exceeded");

        Engine actual_book(unbuffered);
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
        Config exits=unbuffered; exits.exit_on_equilibrium=true; exits.exit_before_funding=false; exits.funding_cashflows=false; exits.perp_maker_bps=0; exits.perp_taker_bps=0; exits.spot_taker_bps=0;
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
              2,"Comparison maker plus three taker rates total 2 bps per cycle");
        fees_exit.on_event(trade(start+1,99940,true));
        close(fees_exit.totals().fees,.004*(99940+99995)*.00005,
              "Open pair pays only its two entry fees; future exit fees are not charged early");
        fees_exit.on_event(book(start+2,99950,99960));
        const auto& closed=fees_exit.pairs()[0];
        const double expected_fees=.004*(99940+99995+99950+100005)*.00005;
        close(fees_exit.totals().fees,expected_fees,"All four entry/exit fees use actual filled notionals");
        close(closed.realized_pnl,-expected_fees,"Convergence can realize a loss after costs");

        Config maker_free=exits; maker_free.perp_maker_bps=0; maker_free.perp_taker_bps=.5; maker_free.spot_taker_bps=.5;
        Engine three_takers(maker_free); seed(three_takers);
        three_takers.on_event(trade(start+1,99940,true));
        close(three_takers.fills()[0].fee,0,"Perp maker entry is free");
        close(three_takers.totals().fees,.004*99995*.00005,"Open pair pays just the spot entry taker fee");
        three_takers.on_event(book(start+2,99950,99960));
        const double three_fees=.004*(99995+99950+100005)*.00005;
        close(three_takers.totals().fees,three_fees,"Exactly three taker fees are charged on a completed cycle");
        close(three_takers.pairs()[0].realized_pnl,-three_fees,"Realized PnL deducts the three actual taker fills");

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
        close(holding.entry_offset_bps,1.5,"Default entry offset is 1.5 bps on either side of equilibrium");
        check(!holding.exit_on_equilibrium && holding.exit_before_funding && holding.max_gross_perp_btc==0,
              "Default policy holds without a cap and exits only before funding");
        close(holding.perp_maker_bps,0,"Default maker execution fee is zero");
        close(holding.perp_taker_bps,.5,"Default perp taker fee is half a basis point");
        close(holding.spot_taker_bps,.5,"Default spot taker fee is half a basis point");
        close(holding.perp_maker_bps+holding.perp_taker_bps+2*holding.spot_taker_bps,1.5,"Default completed-cycle rate is 1.5 bps");
        Engine high(unbuffered); mixed(high);
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
        check(high.pairs()[0].exit_reason=="PRE_FUNDING_PAYER","No convergence exit is hidden in the funding-only policy");
        high.on_event(funding(due,.0002));
        close(high.totals().funding_pnl,.08,"Retained short receives actual settlement funding");
        close(high.totals().realized_pnl,high.pairs()[0].realized_pnl+.08,"Funding receipts are realized immediately while a pair stays open");
        check(!high.premium_ready(),"Settlement resets the premium window rather than reusing future-period data");

        Engine negative(unbuffered); mixed(negative); minutes(negative,-.0008);
        negative.on_event(spot(due-61000)); negative.on_event(book(due-61000,99970,99980));
        negative.on_event(premium(due-60000,-.0008));
        check(!negative.pairs()[0].closed && negative.pairs()[1].closed,"Negative funding closes short payers while retaining long receivers");
        negative.on_event(funding(due,-.0003));
        close(negative.totals().funding_pnl,.12,"Retained long receives negative-rate settlement");

        Engine flat(unbuffered); mixed(flat); minutes(flat,0);
        flat.on_event(spot(due-61000)); flat.on_event(book(due-61000,99970,99980));
        check(!flat.pairs()[0].closed && !flat.pairs()[1].closed,"Flat-region exits wait until the pre-funding window");
        flat.on_event(premium(due-60000,0));
        close(flat.estimated_funding_bps(),1,"Binance flat funding region is +1 bp per eight hours, not zero");
        check(flat.pairs()[0].closed && !flat.pairs()[1].closed,"Flat +1 bp closes long payers and retains short receivers");
        check(flat.pairs()[0].exit_time==due-60000 && flat.fills().size()==6,"Flat-region exit closes both legs at the start of the funding window");
        flat.on_event(funding(due,.0001));
        close(flat.totals().funding_pnl,.04,"Exited long pays no flat funding; retained short receives it");
        close(flat.side_totals()[0].funding_pnl,0,"No funding cash is charged to the exited long pair");
        close(flat.totals().realized_pnl,flat.pairs()[0].realized_pnl+.04,"Exit price result and receiving funding remain separate realized components");
        flat.finish(); check(!flat.pairs()[1].closed,"Replay end leaves funding receivers open");

        Engine neutral(unbuffered); mixed(neutral); minutes(neutral,-.0005);
        neutral.on_event(spot(due-61000)); neutral.on_event(book(due-61000,99970,99980));
        neutral.on_event(premium(due-60000,-.0005));
        close(neutral.estimated_funding_bps(),0,"Exactly -5 bp averaged premium gives zero funding");
        check(!neutral.pairs()[0].closed && !neutral.pairs()[1].closed,"Zero funding does not close either direction");
        for (double value : {-.00049999999999,-.00050000000001}) {
            Engine tiny(unbuffered); mixed(tiny); minutes(tiny,value);
            tiny.on_event(spot(due-61000)); tiny.on_event(book(due-61000,99970,99980));
            tiny.on_event(premium(due-60000,value));
            const bool positive=value>-.0005;
            check(tiny.pairs()[positive?0:1].closed && !tiny.pairs()[positive?1:0].closed,
                  "Any nonzero expected funding selects the payer without a minimum-rate exemption");
            verify_side_attribution(tiny);
        }

        Engine lower(unbuffered); mixed(lower); minutes(lower,-.00045);
        lower.on_event(spot(due-61000)); lower.on_event(book(due-61000,99970,99980));
        lower.on_event(premium(due-60000,-.00045));
        close(lower.estimated_funding_bps(),.5,"Lower linear region can still have a positive funding rate");
        check(lower.pairs()[0].closed && !lower.pairs()[1].closed,"Payer direction follows estimated funding sign, not premium sign");

        Engine gap(unbuffered); mixed(gap);
        for (int i=1;i<=478;++i) if (i!=200) gap.on_event(premium(start+i*60000,.0007));
        gap.on_event(spot(due-61000)); gap.on_event(book(due-61000,99970,99980));
        gap.on_event(premium(due-60000,.0007));
        check(!gap.premium_ready() && !gap.pairs()[0].closed,"Incomplete premium history cannot fabricate a funding-exit forecast");

        Engine jitter(unbuffered);
        jitter.on_event(funding(start+1)); jitter.on_event(spot(start+1)); jitter.on_event(book(start+1));
        jitter.on_event(trade(start+2,99940,true)); minutes(jitter,.0007);
        jitter.on_event(spot(due-61000)); jitter.on_event(book(due-61000));
        jitter.on_event(premium(due-60000,.0007));
        check(jitter.premium_ready() && jitter.pairs()[0].closed,"Millisecond publication jitter does not invalidate premium coverage");
        Engine buffered; seed(buffered);
        check(buffered.decisions().back().side==0,"No quote at -5.5 bps inside the new entry band");
        buffered.on_event(trade(start+1,99940,true));
        check(buffered.fills().empty(),"A print inside the entry band cannot create a pair");
        buffered.on_event(book(start+2,99930.01,99940.01));
        check(buffered.decisions().back().side==0,"A basis just above -6.5 bps is still inside the band");
        buffered.on_event(book(start+3,99930,99940));
        check(buffered.decisions().back().side==1,"At exactly -6.5 bps join the bid");
        buffered.on_event(trade(start+4,99930,true));
        check(buffered.pairs().size()==1 && buffered.fills()[1].side==-1,"Lower boundary entry immediately shorts equal BTC spot");
        buffered.on_event(book(start+5,99930.01,99940.01));
        buffered.on_event(trade(start+6,99930,true));
        check(buffered.pairs().size()==1 && buffered.decisions().back().side==0,"Returning inside the band cancels the remaining quote");
        buffered.on_event(book(start+7,99949,99951));
        check(!buffered.pairs()[0].closed,"Crossing equilibrium still does not exit a buffered position");
        buffered.on_event(book(start+8,99959.99,99969.99));
        check(buffered.decisions().back().side==0,"A basis just below -3.5 bps is still inside the band");
        buffered.on_event(book(start+9,99960,99970));
        check(buffered.decisions().back().side==-1,"At exactly -3.5 bps join the offer");
        buffered.on_event(trade(start+10,99970,false));
        check(buffered.pairs().size()==2 && buffered.fills()[3].side==1,"Upper boundary entry immediately longs equal BTC spot");
        buffered.on_event(book(start+11,99970,99980));
        buffered.on_event(trade(start+12,99980,false));
        check(buffered.pairs().size()==3,"Farther above the upper boundary permits another concurrent pair");
        close(buffered.totals().perp_btc+buffered.totals().spot_btc,0,"Buffered entries preserve equal-BTC hedging");
        buffered.finish();
        check(!buffered.pairs()[0].closed && !buffered.pairs()[1].closed,"No replay-end exit is added by the entry band");
        minutes(buffered,.0007);
        buffered.on_event(spot(due-61000)); buffered.on_event(book(due-61000));
        check(!buffered.pairs()[0].closed,"Buffered entry cannot exit before the funding window");
        buffered.on_event(premium(due-60000,.0007));
        check(buffered.pairs()[0].closed && !buffered.pairs()[1].closed && !buffered.pairs()[2].closed,
              "Funding closes only buffered long payers while retaining short receivers, even inside the entry band");
        for (double offset : {-1.0,10000.0}) {
            Config invalid; invalid.entry_offset_bps=offset;
            bool rejected=false;
            try { Engine bad(invalid); } catch (const std::runtime_error&) { rejected=true; }
            check(rejected,"Invalid entry offsets cannot create an impossible lower basis threshold");
        }
        for (const auto* scenario : {&engine,&convergence,&three_takers,&high,&negative,&flat,&neutral,&lower,&buffered}) verify_side_attribution(*scenario);
        close(high.side_totals()[0].unrealized_pnl,0,"Closed long-perp side has zero unrealized price PnL");
        check(high.side_totals()[0].closed_pairs==1 && high.side_totals()[1].closed_pairs==0,
              "Funding-exited long pair stays in realized attribution while short remains unrealized");
        std::cout << "PASS: -5 bps equilibrium, inclusive -6.5/-3.5 bps entry boundaries, band cancellations, equal-BTC spot hedges,\n"
            << "concurrent pairs, two-sided convergence exits, all four fees, optional recycled cap, legacy no-exit replay,\n"
            << "zero funding, pre-funding cancellations, partial fills, proxy/real spot spreads and per-direction price/funding attribution.\n";
        return 0;
    } catch(const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
