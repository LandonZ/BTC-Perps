#pragma once
#include "events.h"
#include <array>

namespace btc::basis_maker {
using events::Event;
using events::Millis;
struct Config {
    double equilibrium_bps = -5, entry_offset_bps = 1.5;
    double order_btc = 0.01, max_gross_perp_btc = 0, quantity_step = 0.001, tick = 0.1;
    double perp_maker_bps = 0, perp_taker_bps = 0.5, spot_taker_bps = 0.5;
    double proxy_spot_half_spread_bps = 0.5; // Explicit assumption when S events lack actual bid/ask.
    double participation = 0.1;
    double perp_min_notional = 100, spot_min_notional = 5;
    Millis funding_cancel_ms = 60000, max_spot_age_ms = 90000, max_perp_age_ms = 10000;
    bool fill_at_touch = true; // Joining best bid/ask: participation approximates queue allocation.
    bool one_order_per_entry_event = false; // One order budget per fresh threshold crossing.
    bool single_basis_position = false; // At most one signed order-sized position; opposite entries wait until flat.
    bool exit_on_equilibrium = false;
    bool exit_on_opposite_signal = false; // Opposite entry band closes inventory to flat, consuming that signal episode.
    bool retain_decisions = true; // Large archive replays need only the latest quote decision.
    bool retain_fills = true; // Aggregate accounting remains exact without the optional fill ledger.
    bool exit_before_funding = true, funding_exit_payers_only = true, funding_cashflows = true;
    double funding_interest_bps = 1, funding_clamp_bps = 5;
};
struct Fill {
    Millis time;
    std::size_t pair_id;
    std::string leg, liquidity;
    int side;
    double price, btc, notional, fee;
    std::string phase = "ENTRY";
};
struct Pair {
    std::size_t id;
    Millis time;
    int direction;
    double btc, perp_price, spot_price, entry_basis_bps, fees;
    bool closed = false;
    Millis exit_time = -1;
    double exit_perp_price = 0, exit_spot_price = 0, exit_fees = 0, realized_pnl = 0;
    double funding_pnl = 0;
    std::string exit_reason = "";
    double signal_basis_bps = 0; // Midpoint basis used to allow the entry, before executable spot spread.
    std::size_t position_id = 0; // All partial fills between a flat start and the next flat exit.
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
    double open_gross_perp_btc = 0, realized_pnl = 0;
    double net() const { return price_pnl-fees+funding_pnl; }
};
// Each side is a complete hedged pair, never an unhedged perp leg.
struct SideTotals {
    std::size_t entries = 0, closed_pairs = 0;
    double open_btc = 0, unrealized_pnl = 0, realized_price_pnl = 0, funding_pnl = 0, fees = 0;
    double net() const { return unrealized_pnl+realized_price_pnl+funding_pnl; }
};
struct Equity { Millis time; Totals totals; std::array<SideTotals,2> sides; };
struct FillStats {
    std::size_t count = 0,open = 0,max_open = 0;
    double perp_maker_notional = 0,perp_taker_notional = 0,spot_taker_notional = 0;
    double open_gross = 0,max_open_gross = 0;
};
class Engine {
public:
    explicit Engine(Config config = {});
    void on_event(const Event& event, bool decide = true);
    void finish(bool close_positions = false, Millis terminal_time = -1);
    void mark_equity(); // Record a day-end mark without changing resting quotes or inventory.
    Totals totals() const;
    std::array<SideTotals,2> side_totals() const;
    const Config& config() const { return config_; }
    const std::vector<Fill>& fills() const { return fills_; }
    const FillStats& fill_stats() const { return fill_stats_; }
    const std::vector<Pair>& pairs() const { return pairs_; }
    const std::vector<Decision>& decisions() const { return decisions_; }
    const std::vector<Equity>& equity() const { return equity_; }
    double max_drawdown() const { return drawdown_; }
    int spot_proxy_events() const { return proxy_events_; }
    int spot_mid_events() const { return mid_events_; }
    double final_perp_mid() const { return (perp_bid_+perp_ask_)/2; }
    double final_spot_mid() const { return (spot_bid_+spot_ask_)/2; }
    bool premium_ready() const;
    std::size_t entry_events() const { return entry_events_; }
    double max_entry_event_btc() const { return max_entry_event_btc_; }
    double average_premium_bps() const;
    double estimated_funding_bps() const;
private:
    bool ready() const;
    bool funding_guard() const;
    void decide();
    void trade(const Event& event);
    void close_converged();
    void close_opposite_signal();
    void close_before_funding();
    void close_pair(Pair& pair, const std::string& reason);
    void record_fill(Fill fill);
    void refresh_entry_event();
    double used_capacity() const;
    double available_capacity() const;
    double basis() const;
    int entry_side(double observed_basis) const;
    double quantity(double size) const;
    Config config_;
    Millis now_ = -1, spot_time_ = -1, perp_time_ = -1, next_funding_ = -1, last_equity_ = -1;
    double spot_bid_ = 0, spot_ask_ = 0, perp_bid_ = 0, perp_ask_ = 0;
    double quote_price_ = 0, remaining_ = 0;
    int quote_side_ = 0;
    int entry_event_side_ = 0;
    int consumed_exit_signal_ = 0;
    double entry_event_remaining_ = 0, entry_event_filled_ = 0, max_entry_event_btc_ = 0;
    std::size_t entry_events_ = 0;
    std::size_t position_id_ = 0;
    Millis quote_since_ = -1;
    double perp_btc_ = 0, spot_btc_ = 0, gross_btc_ = 0, cash_ = 0, fees_ = 0, peak_ = 0, drawdown_ = 0;
    double open_gross_btc_ = 0, realized_pnl_ = 0;
    double open_long_btc_ = 0, open_short_btc_ = 0, funding_pnl_ = 0;
    Millis premium_period_start_ = -1, premium_time_ = -1;
    double premium_weighted_ = 0, premium_weight_ = 0;
    bool premium_complete_ = true;
    int proxy_events_ = 0, mid_events_ = 0;
    std::vector<Fill> fills_;
    FillStats fill_stats_;
    struct SideAccounting {
        std::size_t entries = 0, closed_pairs = 0;
        double open_entry_cash = 0, open_entry_fees = 0, realized_price_pnl = 0, funding_pnl = 0, fees = 0;
    };
    std::array<SideAccounting,2> sides_; // Long perp / short spot, then short perp / long spot.
    std::vector<Pair> pairs_;
    std::vector<std::size_t> open_pairs_;
    std::vector<Decision> decisions_;
    std::vector<Equity> equity_;
};
void replay(Engine& engine, const std::string& events_file);
void replay_files(Engine& engine, const std::vector<std::string>& events_files, const std::string& premium_file = "");
void write_results(const Engine& engine, const std::string& directory, bool details = false);
} // namespace btc::basis_maker
