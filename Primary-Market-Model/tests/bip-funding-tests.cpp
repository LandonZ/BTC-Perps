#include "../model/bip-funding.h"
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
void check(bool good,const char* message) { if (!good) throw std::runtime_error(message); }
void close(double actual,double expected,const char* message) {
    if (std::abs(actual-expected) > 1e-9) {
        std::cerr << message << ": actual=" << actual << ", expected=" << expected << '\n';
        throw std::runtime_error(message);
    }
}
}
int main() {
    using namespace btc::bip;
    try {
        // Mark fallbacks: VWAP, then mid TWAP, then the other leg plus the previous gap.
        Bucket traded{100*1+106*3,4,999}, quoted{0,0,103}, empty{};
        auto m = marks(traded,quoted,std::nullopt);
        check(m.has_value(),"Both legs observed");
        close(m->futures,104.5,"Futures mark is the 3m VWAP, ignoring the mid");
        close(m->spot,103,"Spot mark falls back to the mid TWAP without trades");
        m = marks(traded,empty,Marks{200,190});
        close(m->spot,104.5-10,"No spot quotes: futures mark plus previous spot-futures gap");
        m = marks(empty,quoted,Marks{200,190});
        close(m->futures,103+10,"No futures quotes: spot mark plus previous futures-spot gap");
        m = marks(empty,empty,Marks{200,190});
        close(m->futures,200,"Nothing observed: carry previous futures mark");
        check(!marks(traded,empty,std::nullopt),"No fallback is possible without previous marks");

        // Premium, smoothing and payment.
        close(premium_bps({{100024,100000}}),0.1,"A 2.4 bp gap is 0.1 bp per hour after dividing by 24");
        close(premium_bps({{100024,100000},{100000,100000}}),0.05,"Premium averages the samples");
        close(funding_rate_bps(0.1,0.2),0.125,"0.75 * premium + 0.25 * previous");
        close(funding_rate_bps(premium_bps({{100024,100000}}),0.2),0.125,"A 2.4 bp gap is a 0.1 bp hourly premium");
        close(payment_usd(1,100000,10),1.0,"Coinbase example: 1 contract, $100k, 0.1% -> $1.00 long debit");
        close(payment_usd(-1,100000,10),-1.0,"The short receives the same amount");
        close(payment_usd(3,100000,-0.1),-0.03,"Negative funding credits longs");

        // Streaming estimator over one full hour.
        const Millis t0 = 1760000400000; // An exact UTC hour (2025-10-09T09:00:00Z).
        check(t0 % hour_ms == 0,"Test hour is aligned");
        FundingEstimator e(0.4);
        close(e.projected_rate_bps(),0.1,"Before any sample the projection is 0.25 * previous");
        e.advance(t0);
        e.on_quote(FundingEstimator::Spot,t0,99,101);              // mid 100 for 1 minute
        e.on_quote(FundingEstimator::Spot,t0+60000,105,107);       // mid 106 for 2 minutes
        e.on_trade(FundingEstimator::Futures,t0+1000,105,1);
        e.on_trade(FundingEstimator::Futures,t0+2000,107,3);       // VWAP 106.5
        check(!e.advance(t0+bucket_ms),"Closing the first bucket publishes nothing");
        check(e.samples_this_hour() == 1,"One 3-minute sample");
        const double first = (106.5-104)/104/24*10000;
        close(e.projected_rate_bps(),0.75*first+0.25*0.4,"Projection uses VWAP futures and mid-TWAP spot");
        // Remaining 19 buckets: futures has no trades and no quotes, spot mid stays 106.
        const auto rate = e.advance(t0+hour_ms);
        check(rate.has_value() && e.samples_this_hour() == 0,"A full hour publishes and resets");
        const double later = (106+2.5-106)/106/24*10000; // Previous gap carried forward.
        close(*rate,0.75*(first+19*later)/20+0.25*0.4,"Twenty samples averaged, then smoothed");
        close(e.previous_rate_bps(),*rate,"Published rate becomes the previous rate");

        // Futures closed for a full hour: no rate, previous rate unchanged.
        check(!e.advance(t0+2*hour_ms),"No futures data all hour publishes nothing");
        close(e.previous_rate_bps(),*rate,"Previous rate kept through a closed hour");

        // Starting mid-hour discards the incomplete first hour.
        FundingEstimator late(0);
        late.on_quote(FundingEstimator::Spot,t0+30*60000,100,100);
        late.on_quote(FundingEstimator::Futures,t0+30*60000,101,101);
        check(!late.advance(t0+hour_ms),"Partial first hour is not published");
        check(late.advance(t0+2*hour_ms).has_value(),"The next full hour is published");
        close(late.previous_rate_bps(),0.75*(1.0/100/24*10000),"Full-hour rate from constant books");

        bool threw = false;
        try { late.advance(t0); } catch (const std::runtime_error&) { threw = true; }
        check(threw,"Out-of-order data is rejected");

        std::cout << "PASS: BIP mark fallbacks, 3m/20-sample premium, 0.75 smoothing, contract\n"
            << "payments, live estimator hours, closed-market and partial-hour handling.\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
