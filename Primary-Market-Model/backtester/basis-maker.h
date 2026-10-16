#pragma once
#include "maker-backtest.h"

namespace btc::basis_maker {
using maker_backtest::Event;
using maker_backtest::Millis;
struct Config {
    double equilibrium_bps = -5;
    double order_btc = 0.01, max_gross_perp_btc = 0.05, quantity_step = 0.001, tick = 0.1;
    double perp_maker_bps = 2, spot_taker_bps = 10;
    double proxy_spot_half_spread_bps = 0.5; // Explicit assumption when S events lack actual bid/ask.
    double participation = 0.1;
    double perp_min_notional = 100, spot_min_notional = 5;
    Millis funding_cancel_ms = 60000, max_spot_age_ms = 90000, max_perp_age_ms = 10000;
    bool fill_at_touch = true; // Joining best bid/ask: participation approximates queue allocation.
};
struct Fill {
    Millis time;
    std::size_t pair_id;
    std::string leg, liquidity;
    int side;
    double price, btc, notional, fee;
};
struct Pair {
    std::size_t id;
    Millis time;
    int direction;
    double btc, perp_price, spot_price, entry_basis_bps, fees;
};
struct Decision {
    Millis time;
    std::string reason;
    int side;
    double perp_mid_basis_bps, price, remaining;
};
struct Totals {
    double price_pnl = 0, fees = 0, funding_pnl = 0;
    double perp_btc = 0, spot_btc = 0, gross_perp_btc = 0;
    double net() const { return price_pnl-fees+funding_pnl; }
};
struct Equity { Millis time; Totals totals; };
class Engine {
public:
    explicit Engine(Config config = {});
    void on_event(const Event& event, bool decide = true);
    void finish();
    Totals totals() const;
    const Config& config() const { return config_; }
    const std::vector<Fill>& fills() const { return fills_; }
    const std::vector<Pair>& pairs() const { return pairs_; }
    const std::vector<Decision>& decisions() const { return decisions_; }
    const std::vector<Equity>& equity() const { return equity_; }
    double max_drawdown() const { return drawdown_; }
    int spot_proxy_events() const { return proxy_events_; }
    int spot_mid_events() const { return mid_events_; }
private:
    bool ready() const;
    bool funding_guard() const;
    void decide();
    void trade(const Event& event);
    double basis() const;
    double quantity(double size) const;
    Config config_;
    Millis now_ = -1, spot_time_ = -1, perp_time_ = -1, next_funding_ = -1, last_equity_ = -1;
    double spot_bid_ = 0, spot_ask_ = 0, perp_bid_ = 0, perp_ask_ = 0;
    double quote_price_ = 0, remaining_ = 0;
    int quote_side_ = 0;
    Millis quote_since_ = -1;
    double perp_btc_ = 0, spot_btc_ = 0, gross_btc_ = 0, cash_ = 0, fees_ = 0, peak_ = 0, drawdown_ = 0;
    int proxy_events_ = 0, mid_events_ = 0;
    std::vector<Fill> fills_;
    std::vector<Pair> pairs_;
    std::vector<Decision> decisions_;
    std::vector<Equity> equity_;
};
void replay(Engine& engine, const std::string& events_file);
void write_results(const Engine& engine, const std::string& directory);
} // namespace btc::basis_maker
