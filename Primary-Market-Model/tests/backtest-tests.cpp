#include "../backtester/basis-backtest.h"
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
void check(bool okay,const char* message) { if (!okay) throw std::runtime_error(message); }
void close(double actual,double expected,const char* message) {
    if (std::abs(actual-expected) > 1e-8) {
        std::cerr << message << ": " << actual << " != " << expected << '\n';
        throw std::runtime_error(message);
    }
}
btc::backtest::Dataset fixture() {
    btc::backtest::Dataset data;
    constexpr btc::backtest::Time first = 1780444800;
    for (int minute = 0; minute <= 180; ++minute)
        data.prices.emplace(first+minute*60,btc::backtest::Prices{100,102,100,102});
    for (int hour = 0; hour <= 3; ++hour)
        data.funding.push_back({first+hour*3600,0.0001,102});
    return data;
}
}
int main() {
    using namespace btc::backtest;
    try {
        auto data = fixture();
        Settings settings;
        settings.strategy = Strategy::TheoCarry; // Verify the original general theo-carry engine separately.
        settings.quantity_btc = 1; settings.holding_minutes = 120;
        const auto stable = run(data,settings);
        check(stable.trades.size() == 1,"One pair at a time, no zero-length trade at data end");
        const auto& trade = stable.trades.front();
        check(trade.direction == -1,"Positive forecast funding selects short perp / long spot");
        check(trade.funding_payments == 2,"Exclude entry funding, include exit funding");
        close(trade.price_pnl,0,"Unchanged basis prices have zero price PnL");
        close(trade.funding_pnl,0.0204,"Realized funding uses actual rates and event marks");
        close(trade.net_bps,2.04,"Normalize trade bps to entry spot notional");
        close(stable.equity.back().net_pnl,stable.net_pnl,"Equity reconciles to completed trade totals");

        auto changed = data;
        auto& final = changed.prices.rbegin()->second;
        final.spot_open = 110; final.perp_open = 113;
        changed.funding.back().rate = -0.002;
        const auto later = run(changed,settings);
        const auto& realized = later.trades.front();
        check(realized.entry == trade.entry && realized.direction == trade.direction,
              "Future funding and prices must not change the existing entry");
        close(realized.forecast_edge_bps,trade.forecast_edge_bps,"Forecast uses no future exit or rate");
        close(realized.price_pnl,-1,"Exact paired PnL cancels common spot/perp price movement");
        close(realized.funding_pnl,0.0102-0.204,"A future funding sign reversal changes realized cash flows");
        close(later.net_pnl,-1.1938,"Basis loss and actual funding reconcile");
        check(later.max_drawdown > 1,"Drawdown includes open-position basis risk and funding");

        auto missing = data;
        missing.prices.erase(trade.exit);
        missing.prices.emplace(trade.exit+60,Prices{100,102,100,102});
        // Keep funding coverage complete beyond the delayed exit.
        missing.funding.push_back({data.funding.back().time+3600,0.0001,102});
        const auto delayed = run(missing,settings);
        check(delayed.delayed_exits == 1,"Missing exit candle delays execution until the next matched price");
        close(delayed.net_pnl,stable.net_pnl,"Do not fabricate or zero-fill missing prices");

        auto gap = data;
        gap.prices.erase(gap.prices.begin()->first+30*60);
        const auto skipped = run(gap,settings);
        check(skipped.skipped_feature_hours >= 2,"Incomplete trailing hour cannot enter using future interpolation");
        check(skipped.trades.size() == 1 && skipped.trades.front().forced_end,
              "A position still open at data end is explicitly liquidated");
        check(skipped.trades.front().entry == data.prices.begin()->first+7200,
              "Entry waits until a complete trailing hour is available");

        auto negative = data;
        for (auto& [t,p] : negative.prices) { (void)t; p.perp_open = p.perp_close = 98; }
        for (auto& f : negative.funding) { f.rate = -0.0001; f.mark = 98; }
        const auto long_pair = run(negative,settings);
        check(long_pair.trades.front().direction == 1,"Negative forecast funding selects long perp / short spot");
        close(long_pair.funding_pnl,0.0196,"Long basis receives negative realized funding");

        auto broken = data;
        broken.funding.erase(broken.funding.begin()+1);
        bool rejected = false;
        try { (void)run(broken,settings); } catch (const std::runtime_error&) { rejected = true; }
        check(rejected,"Missing funding must fail rather than fabricate zero payments");
        settings.minimum_edge_bps = 100000;
        const auto no_trade = run(data,settings);
        check(no_trade.trades.empty(),"Respect the optional minimum predicted edge");
        close(no_trade.net_pnl,0,"No-position PnL is zero");

        // Latest strategy: long a discounted perp, take the first positive observed
        // pair PnL, converge to -5 bps, or stop after five minutes.
        Settings scalp;
        scalp.strategy = Strategy::LongBasis;
        scalp.quantity_btc = 1;
        auto discount = fixture();
        const Time entry = discount.prices.begin()->first+3600;
        for (auto& [t,p] : discount.prices) { (void)t; p.perp_open = p.perp_close = 99.90; }
        for (auto& f : discount.funding) f.rate = 0;
        discount.prices[entry+60].perp_open = 99.93;
        const auto profitable = run(discount,scalp);
        const auto& first_trade = profitable.trades.front();
        check(first_trade.entry == entry && first_trade.direction == 1,"Buy only a perp discounted by more than five bps");
        check(first_trade.exit == entry+60 && first_trade.exit_reason == "PROFIT",
              "Exit at first positive observed PnL even before the convergence threshold");
        close(first_trade.net_bps,3,"A narrowing from minus ten to minus seven earns three bps");
        check(first_trade.exit_basis_bps < -5,"Profit-taking need not wait until minus five");
        for (const auto& tr : profitable.trades) {
            check(tr.direction == 1,"Never short the basis in the latest strategy");
            check(tr.exit-tr.entry <= 300,"All measured trades respect the five-minute cap");
        }

        scalp.exit_on_profit = false;
        discount.prices[entry+60].perp_open = 99.95;
        const auto target = run(discount,scalp);
        check(target.trades.front().exit_reason == "CONVERGED","Close when discount falls to five bps or less");
        discount.prices[entry+60].perp_open = 99.90;
        const auto timeout = run(discount,scalp);
        check(timeout.trades.front().exit == entry+300 && timeout.trades.front().exit_reason == "TIMEOUT",
              "Flat or losing trades must close at the five-minute deadline");
        discount.prices.erase(entry+300);
        const auto unmeasurable = run(discount,scalp);
        check(unmeasurable.excluded_missing_deadline >= 1,"Exclude unknown deadline fills rather than overrun the cap");
        close(unmeasurable.equity.back().net_pnl,unmeasurable.net_pnl,
              "Excluded trades leave no invented PnL in published equity");

        auto threshold = fixture();
        for (auto& [t,p] : threshold.prices) { (void)t; p.perp_open = p.perp_close = 99.95; }
        check(run(threshold,scalp).trades.empty(),"Exact five-bp discount does not meet the strict entry condition");
        auto boundary = threshold;
        for (auto& [t,p] : boundary.prices) { (void)t; p.perp_open = p.perp_close = 99.96; }
        const Time late_entry = boundary.prices.begin()->first+118*60;
        for (int minute = 0; minute <= 5; ++minute) boundary.prices[late_entry+minute*60].perp_open = 99.90;
        for (auto& f : boundary.funding) { f.rate = -0.0001; f.mark = 99.90; }
        scalp.exit_on_profit = true;
        const auto receipt = run(boundary,scalp);
        check(receipt.trades.front().exit_reason == "PROFIT" && receipt.trades.front().funding_payments == 1,
              "A funding receipt can trigger the profitable exit at the hourly boundary");
        close(receipt.trades.front().funding_pnl,0.00999,"Include actual funding across a five-minute window");

        // Mirror the long-side data around the fixed spot price. With zero
        // realized funding, matched short trades must earn identical price PnL.
        auto mirror = fixture();
        for (auto& [t,p] : mirror.prices) {
            (void)t; p.perp_open = p.perp_close = 100.10;
        }
        mirror.prices[entry+60].perp_open = 100.07;
        for (auto& f : mirror.funding) f.rate = 0;
        Settings both;
        both.quantity_btc = 1;
        const auto short_profit = run(mirror,both);
        check(short_profit.trades.front().direction == -1 && short_profit.trades.front().exit_reason == "PROFIT",
              "Short a premium above five bps and exit at the first observed profit");
        close(short_profit.trades.front().net_bps,3,"Premium narrowing from ten to seven earns three bps");
        check(short_profit.trades.size() == profitable.trades.size(),"Mirrored data produces matching trade counts");
        for (std::size_t i = 0; i < profitable.trades.size(); ++i) {
            check(short_profit.trades[i].entry == profitable.trades[i].entry &&
                  short_profit.trades[i].exit == profitable.trades[i].exit,"Mirrored entry and exit timing is symmetric");
            close(short_profit.trades[i].net_pnl,profitable.trades[i].net_pnl,"Both sides reconcile under a mirrored price path");
        }
        close(short_profit.equity.back().net_pnl,short_profit.net_pnl,"Short-side equity reconciles to the ledger");
        both.exit_on_profit = false;
        mirror.prices[entry+60].perp_open = 100.05;
        check(run(mirror,both).trades.front().exit_reason == "CONVERGED","Short side converges at plus five bps");
        for (auto& [t,p] : mirror.prices) { (void)t; p.perp_open = p.perp_close = 100.05; }
        check(run(mirror,both).trades.empty(),"Exact plus-five threshold does not enter");

        // Positive funding is a receipt for the short side at an hourly boundary.
        auto premium_boundary = boundary;
        for (auto& [t,p] : premium_boundary.prices) {
            (void)t; p.perp_open = 200-p.perp_open; p.perp_close = 200-p.perp_close;
        }
        for (auto& f : premium_boundary.funding) { f.rate = 0.0001; f.mark = 100.10; }
        both.exit_on_profit = true;
        const auto short_receipt = run(premium_boundary,both);
        check(short_receipt.trades.front().direction == -1 && short_receipt.trades.front().funding_payments == 1,
              "Short basis owns the positive funding payment across the boundary");
        close(short_receipt.trades.front().funding_pnl,0.01001,"Recorded positive funding credits the short basis");

        auto mixed = fixture();
        for (auto& [t,p] : mixed.prices) { (void)t; p.perp_open = p.perp_close = 100; }
        for (auto& f : mixed.funding) f.rate = 0;
        mixed.prices[entry].perp_open = 99.90;
        mixed.prices[entry+120].perp_open = 100.10;
        const auto combined = run(mixed,both);
        check(combined.long_trades == 1 && combined.short_trades == 1,"A combined run takes both sides without overlapping positions");
        close(combined.net_pnl,0.2,"Combined long and short profits sum correctly");
        Settings short_only = both; short_only.strategy = Strategy::ShortBasis;
        const auto restricted = run(mixed,short_only);
        check(restricted.long_trades == 0 && restricted.short_trades == 1,"Short-only comparison excludes long entries");
        mixed.prices[entry+60].perp_open = 99.80;
        const auto different_future = run(mixed,both);
        check(different_future.trades.front().entry == combined.trades.front().entry &&
              different_future.trades.front().direction == combined.trades.front().direction,
              "Combined strategy does not use future prices to choose its first direction");
        scalp.holding_minutes = 6; rejected = false;
        try { (void)run(boundary,scalp); } catch (const std::runtime_error&) { rejected = true; }
        check(rejected,"Reject a holding cap longer than five minutes for the latest strategy");
        std::cout << "PASS: causal entries, exact hedge PnL, actual funding timing/signs,\n"
                  << "symmetric basis threshold/profit exits, five-minute cap, missing-data handling, and drawdown.\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
