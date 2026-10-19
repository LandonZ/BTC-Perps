#include "bip-funding.h"
#include <cmath>
#include <stdexcept>

namespace btc::bip {
namespace {
constexpr double bps = 10000.0;
void finite(double value, const char* label) {
    if (!std::isfinite(value)) throw std::runtime_error(label);
}
double own_mark(const Bucket& b) {
    if (b.traded_qty > 0) return b.traded_notional / b.traded_qty;
    return b.mid_twap; // 0 when there were no quotes either.
}
Millis floor_to(Millis time, Millis step) { return time - ((time % step) + step) % step; }
} // namespace

std::optional<Marks> marks(const Bucket& f, const Bucket& s, std::optional<Marks> prev) {
    for (double v : {f.traded_notional, f.traded_qty, f.mid_twap, s.traded_notional, s.traded_qty, s.mid_twap}) {
        finite(v, "Bucket inputs must be finite");
        if (v < 0) throw std::runtime_error("Bucket inputs must be nonnegative");
    }
    Marks m{own_mark(f), own_mark(s)};
    if (m.futures > 0 && m.spot > 0) return m;
    if (!prev) return std::nullopt;
    if (m.futures > 0) m.spot = m.futures + (prev->spot - prev->futures);
    else if (m.spot > 0) m.futures = m.spot + (prev->futures - prev->spot);
    else m = *prev; // Neither leg traded or quoted: carry the previous marks.
    return m;
}

double premium_bps(const std::vector<Marks>& samples) {
    if (samples.empty()) throw std::runtime_error("Premium needs at least one 3-minute sample");
    double sum = 0;
    for (const auto& m : samples) {
        if (!(m.futures > 0 && m.spot > 0)) throw std::runtime_error("Marks must be positive");
        sum += (m.futures - m.spot) / m.spot / 24;
    }
    return sum / samples.size() * bps;
}

double funding_rate_bps(double premium, double previous) {
    finite(premium, "Premium must be finite"); finite(previous, "Previous funding must be finite");
    return alpha * premium + (1 - alpha) * previous;
}

double payment_usd(double contracts, double mark, double rate) {
    for (double v : {contracts, mark, rate}) finite(v, "Payment inputs must be finite");
    if (mark <= 0) throw std::runtime_error("Funding mark must be positive");
    return contracts * contract_btc * mark * rate / bps;
}

FundingEstimator::FundingEstimator(double previous) : previous_rate_(previous) {
    finite(previous, "Previous funding must be finite");
}

void FundingEstimator::accrue_mid(LegState& leg, Millis until) {
    if (leg.mid <= 0 || until <= leg.mid_since) return;
    const double dt = static_cast<double>(until - leg.mid_since);
    leg.mid_sum += leg.mid * dt; leg.mid_weight_ms += dt;
    leg.mid_since = until;
}

void FundingEstimator::on_trade(Leg leg, Millis time, double price, double qty) {
    finite(price, "Trade price must be finite"); finite(qty, "Trade size must be finite");
    if (price <= 0 || qty <= 0) throw std::runtime_error("Trade price and size must be positive");
    advance(time);
    auto& b = legs_[leg].bucket;
    b.traded_notional += price * qty; b.traded_qty += qty;
}

void FundingEstimator::on_quote(Leg leg, Millis time, double bid, double ask) {
    finite(bid, "Bid must be finite"); finite(ask, "Ask must be finite");
    if (bid <= 0 || ask < bid) throw std::runtime_error("Quotes need 0 < bid <= ask");
    advance(time);
    auto& l = legs_[leg];
    accrue_mid(l, time);
    l.mid = (bid + ask) / 2; l.mid_since = time;
}

void FundingEstimator::close_bucket() {
    const Millis end = bucket_start_ + bucket_ms;
    Bucket closed[2];
    for (int i = 0; i < 2; ++i) {
        auto& l = legs_[i];
        accrue_mid(l, end);
        closed[i] = l.bucket;
        closed[i].mid_twap = l.mid_weight_ms > 0 ? l.mid_sum / l.mid_weight_ms : 0;
        // The book persists into the next bucket; trades and weights restart.
        l.bucket = {}; l.mid_sum = l.mid_weight_ms = 0;
        if (l.mid > 0) l.mid_since = end;
    }
    if (own_mark(closed[Futures]) > 0) futures_seen_ = true;
    if (auto m = marks(closed[Futures], closed[Spot], last_marks_)) {
        samples_.push_back(*m); last_marks_ = m;
    }
    bucket_start_ = end;
}

std::optional<double> FundingEstimator::advance(Millis time) {
    if (bucket_start_ < 0) {
        bucket_start_ = floor_to(time, bucket_ms);
        partial_hour_ = time != floor_to(time, hour_ms);
        return std::nullopt;
    }
    if (time < bucket_start_) throw std::runtime_error("Market data must arrive in timestamp order");
    std::optional<double> published;
    while (time >= bucket_start_ + bucket_ms) {
        close_bucket();
        if (bucket_start_ % hour_ms == 0) { // That bucket ended the hour.
            // A closed futures market publishes nothing. A partial first hour is
            // discarded; supply the venue's previous rate to the constructor.
            if (futures_seen_ && !samples_.empty() && !partial_hour_) {
                previous_rate_ = funding_rate_bps(premium_bps(samples_), previous_rate_);
                published = previous_rate_;
            }
            samples_.clear(); partial_hour_ = futures_seen_ = false;
        }
    }
    return published;
}

double FundingEstimator::projected_rate_bps() const {
    return samples_.empty() ? (1 - alpha) * previous_rate_
                            : funding_rate_bps(premium_bps(samples_), previous_rate_);
}
} // namespace btc::bip
