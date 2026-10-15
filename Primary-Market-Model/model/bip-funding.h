#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <vector>

// Coinbase Derivatives nano BTC perp-style future (BIP-20DEC30-CDE) funding.
// Source: Coinbase help centre, "US Perpetual-Style Futures Funding Rate".
//   Premium = TWAP([(futures mark - spot mark) / spot mark / 24], 1 hour, 3 min)
//   Rate    = 0.75 * Premium + 0.25 * Previous rate      (no cap is published)
//   Payment = contracts * 0.01 BTC * mark * rate          (positive: long pays)
namespace btc::bip {
using Millis = std::int64_t;
constexpr double contract_btc = 0.01;
constexpr double price_tick = 5; // USD per BTC index point; $0.05 per contract.
constexpr double alpha = 0.75;
constexpr Millis bucket_ms = 3 * 60 * 1000, hour_ms = 60 * 60 * 1000;
constexpr int buckets_per_hour = 20;

// One leg (futures or spot) over one 3-minute bucket. Zero means "not observed".
struct Bucket {
    double traded_notional = 0, traded_qty = 0; // For the 3m VWAP.
    double mid_twap = 0;                        // 3m TWAP of the best bid/offer midpoint.
};
struct Marks { double futures = 0, spot = 0; };

// Applies the published fallbacks: VWAP, else mid TWAP, else the other leg's mark
// plus the previous gap. Empty when neither leg has trades or quotes and no
// previous marks exist.
std::optional<Marks> marks(const Bucket& futures, const Bucket& spot, std::optional<Marks> previous);
// Mean of (F - S) / S / 24 over the supplied samples, in bps. Requires >= 1 sample.
double premium_bps(const std::vector<Marks>& samples);
double funding_rate_bps(double premium_bps, double previous_rate_bps);
// Cash paid by a LONG position (negative = received). Contracts may be negative (short).
double payment_usd(double contracts, double mark, double rate_bps);

// Streaming estimator for live/paper trading. Feed trades and top-of-book quotes
// for both legs in timestamp order; call advance() with the current time.
class FundingEstimator {
public:
    enum Leg { Futures = 0, Spot = 1 };
    explicit FundingEstimator(double previous_rate_bps = 0);
    void on_trade(Leg leg, Millis time, double price, double qty);
    void on_quote(Leg leg, Millis time, double bid, double ask);
    // Closes every bucket that ended at or before `time`. Returns the final rate
    // when an hour completes; empty if no hour completed, the futures leg had no
    // trades or quotes all hour (market closed: Coinbase publishes no rate),
    // or the estimator started part-way through that hour (incomplete TWAP).
    std::optional<double> advance(Millis time);
    // Rate implied by the completed buckets of the current hour (Coinbase also
    // publishes a projected rate). Equals 0.25 * previous before any bucket closes.
    double projected_rate_bps() const;
    double previous_rate_bps() const { return previous_rate_; }
    int samples_this_hour() const { return static_cast<int>(samples_.size()); }

private:
    struct LegState {
        Bucket bucket;
        double mid = 0, mid_weight_ms = 0, mid_sum = 0;
        Millis mid_since = 0;
    };
    void accrue_mid(LegState& leg, Millis until);
    void close_bucket();
    std::array<LegState, 2> legs_{};
    std::vector<Marks> samples_;
    std::optional<Marks> last_marks_;
    double previous_rate_ = 0;
    Millis bucket_start_ = -1;
    bool partial_hour_ = false, futures_seen_ = false;
};
} // namespace btc::bip
