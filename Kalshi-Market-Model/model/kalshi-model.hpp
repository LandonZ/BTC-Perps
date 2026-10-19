// Naive basis model for the Kalshi BTC perp (KXBTCPERP).
//
// Basis = perp price / spot price - 1, in bps. The model assumes the basis
// reverts to a centre, and trades whenever the quote it can actually hit is
// further from that centre than `edge_bps`:
//   - perp ask cheap enough  -> buy the perp  (hedge: sell spot)
//   - perp bid rich enough   -> sell the perp (hedge: buy spot)
// It exits when the basis is back at the centre.
#pragma once

#include <cstdint>
#include <deque>

namespace kalshi {

constexpr double BPS = 1e-4;

// One observation: Kalshi top of book and BTC spot, all in USD per BTC.
struct Quote {
    int64_t ts = 0;
    double bid = 0, ask = 0, spot = 0;

    double basis_bid() const { return (bid / spot - 1) / BPS; }  // what we get selling the perp
    double basis_ask() const { return (ask / spot - 1) / BPS; }  // what we pay buying the perp
    double basis_mid() const { return ((bid + ask) / 2 / spot - 1) / BPS; }
    double spread_bps() const { return (ask - bid) / spot / BPS; }
};

struct ModelParams {
    int lookback = 1440;    // bars in the rolling-mean centre; 0 = use centre_bps as a fixed centre
    double centre_bps = 0;  // fixed centre, only used when lookback == 0
    double edge_bps = 3;    // enter when the hittable basis is this far through the centre
    double exit_bps = 0;    // exit when the basis is back within this of the centre
};

enum class Action { Hold, Buy, Sell };  // side of the perp order

class NaiveBasisModel {
public:
    explicit NaiveBasisModel(const ModelParams& p) : p_(p) {}

    // position: +1 long perp, -1 short perp, 0 flat.
    Action on_quote(const Quote& q, int position);

    bool ready() const { return p_.lookback == 0 || (int)window_.size() >= p_.lookback; }
    double centre_bps() const;

private:
    ModelParams p_;
    std::deque<double> window_;
    double sum_ = 0;
};

}  // namespace kalshi
