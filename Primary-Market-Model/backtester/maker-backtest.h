#pragma once
#include "../model/maker-model.h"
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace btc::maker_backtest {
using Millis = std::int64_t;
struct Config {
    maker::Config pricing;
    double order_btc = 0.01, max_inventory_btc = 0.05, quantity_step = 0.001;
    double min_notional = 100, participation = 0.1;
    Millis latency_ms = 0, reprice_ms = 0, max_spot_age_ms = 90000, max_book_age_ms = 10000;
    bool fill_at_touch = false;
};
struct Event {
    Millis time = 0;
    char kind = 0; // S spot close, Q perp book, T perp trade, F settlement, R prior settled rate.
    double price = 0, quantity = 0, bid = 0, ask = 0;
    bool buyer_maker = false;
    double funding_rate = 0, interval_hours = 8;
};
struct Fill { Millis time; int side; double price, btc, fee, inventory; };
struct Decision { Millis time; maker::Quotes quotes; double funding_bps, inventory; };
struct Equity { Millis time; double price_pnl, funding, fees, net, inventory; };

class Engine {
public:
    explicit Engine(Config config = {});
    void on_event(const Event& event, bool decide = true);
    void finish();
    const Config& config() const { return config_; }
    const std::vector<Fill>& fills() const { return fills_; }
    const std::vector<Decision>& decisions() const { return decisions_; }
    const std::vector<Equity>& equity() const { return equity_; }
    Equity totals() const;
    double drawdown() const { return drawdown_; }
    int post_only_rejections() const { return rejections_; }
    int spot_proxy_events() const { return spot_proxy_events_; }
    int spot_mid_events() const { return spot_mid_events_; }
private:
    struct Quote { double price = 0, remaining = 0; Millis since = 0; };
    struct Pending { Millis due; Quote quote; };
    double quantity(double size) const;
    void activate(Millis time, bool inclusive = false);
    void decide();
    void trade(const Event& event);
    Config config_;
    Millis now_ = -1, spot_time_ = -1, book_time_ = -1, rate_time_ = -1;
    Millis next_funding_ = -1, last_decision_ = -1, last_equity_ = -1;
    double spot_ = 0, bid_ = 0, ask_ = 0, rate_bps_ = 0, interval_minutes_ = 480;
    double inventory_ = 0, cash_ = 0, funding_ = 0, fees_ = 0, peak_ = 0, drawdown_ = 0;
    int rejections_ = 0;
    int spot_proxy_events_ = 0, spot_mid_events_ = 0;
    Quote live_[2];
    std::optional<Pending> pending_[2];
    std::vector<Fill> fills_;
    std::vector<Decision> decisions_;
    std::vector<Equity> equity_;
};
// Rejects malformed or unsorted input; decisions are deferred across equal timestamps.
void replay(Engine& engine, const std::string& events_file);
void read_events(const std::string& events_file, const std::function<void(const Event&,bool)>& feed);
void write_results(const Engine& engine, const std::string& directory);
} // namespace btc::maker_backtest
