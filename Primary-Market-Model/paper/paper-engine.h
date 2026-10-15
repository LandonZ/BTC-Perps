#pragma once
#include "../model/bip-funding.h"
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

// Event-driven paper trader for the Coinbase BIP perp-style future against
// BTC-USD spot. The same Engine runs recorded ticks (backtest), minute candles,
// and a live stream piped from tools/record_coinbase.py, so every mode shares
// one set of decision and fill rules. No lookahead: decisions use only events
// already applied, and orders reach the market `latency_ms` later.
namespace btc::paper {
using Millis = std::int64_t;
enum Leg { Perp = 0, Spot = 1 };

struct Event {
    Millis time = 0;  // Arrival time on our machine (recv_ms).
    char kind = 'Q';  // 'Q' top-of-book, 'T' trade.
    Leg leg = Perp;
    double bid = 0, bid_qty = 0, ask = 0, ask_qty = 0; // Quotes.
    double price = 0, size = 0;                        // Trades.
};

// Reads the recorder's events.csv: recv_ms,exch_ms,kind,product,bid,bid_qty,ask,ask_qty,price,size,side.
// Header rows (also repeated ones from concatenated days) and other products are skipped.
class EventReader {
public:
    EventReader(std::istream& in, std::string perp_product, std::string spot_product);
    bool next(Event& event);
    std::size_t skipped() const { return skipped_; }
private:
    std::istream& in_;
    std::string perp_, spot_;
    std::size_t skipped_ = 0;
};

enum class Mode {
    Taker, // Allen's basis rule on executable prices: cross both legs, exit on profit/convergence/timeout.
    Maker  // Naive market making: quote the perp around a spot-derived theo; no hedge yet.
};

struct Config {
    Mode mode = Mode::Taker;
    Millis latency_ms = 50;           // Decision -> order live/filled at the exchange.
    double threshold_bps = 5;         // Taker entry band; maker quote half-width.
    double center_bps = 0;            // Fixed fair basis when ema_halflife_min is 0.
    double ema_halflife_min = 0;      // >0: fair basis = causal EMA of the mid basis.
    int contracts = 1;                // 0.01 BTC each; the spot hedge is the same BTC.
    // Taker exits.
    Millis max_hold_ms = 5 * 60 * 1000;
    bool exit_on_profit = true;       // First positive PnL net of fees (including the exit fee).
    // Maker quoting.
    int max_inventory_contracts = 5;
    double skew_bps_per_contract = 1; // Reservation price moves against inventory.
    bool fill_at_touch = false;       // false: a trade must print THROUGH our price to fill us.
    // Costs. Coinbase BIP charges a per-contract fee; spot is bps of notional.
    double perp_taker_bps = 0, perp_maker_bps = 0, perp_fee_usd_per_contract = 0;
    double spot_taker_bps = 0;
    double previous_funding_bps = 0;  // Seed for the hourly BIP funding estimator.
};

struct Totals {
    double price_pnl = 0, funding_pnl = 0, fees = 0;
    double net() const { return price_pnl + funding_pnl - fees; }
};
struct Fill { Millis time = 0; Leg leg = Perp; int side = 0; double price = 0, btc = 0, fee = 0; int inventory = 0; double basis_bps = 0; };
struct RoundTrip {
    Millis entry = 0, exit = 0;
    int direction = 0; // +1 long perp / short spot, -1 short perp / long spot.
    double entry_spot = 0, entry_perp = 0, exit_spot = 0, exit_perp = 0;
    double signal_basis_bps = 0, entry_basis_bps = 0, exit_basis_bps = 0; // Signal vs fill shows latency slippage.
    double price_pnl = 0, funding_pnl = 0, fees = 0, net_pnl = 0, net_bps = 0;
    int funding_payments = 0;
    bool forced_end = false;
    std::string exit_reason;
};
struct EquityPoint { Millis time = 0; Totals totals; int position = 0; };

class Engine {
public:
    explicit Engine(Config config);
    // `decide` = false defers the decision while more events share this timestamp
    // (e.g. both legs of a candle), so the strategy never sees a half-updated state.
    void on_event(const Event& event, bool decide = true);
    // Closes a taker position at the current book (forced_end) and records final equity.
    void finish();

    const Config& config() const { return config_; }
    Totals totals() const; // Including open-position mark-to-market at mids.
    const std::vector<RoundTrip>& trips() const { return trips_; }
    const std::vector<Fill>& fills() const { return fills_; }
    const std::vector<EquityPoint>& equity() const { return equity_; }
    double max_drawdown() const;
    int position_contracts() const;
    Millis now() const { return now_; }
    std::optional<double> mid_basis_bps() const;
    double center_bps() const { return center_; }
    double projected_funding_bps() const { return funding_.projected_rate_bps(); }

private:
    struct Top { double bid = 0, bid_qty = 0, ask = 0, ask_qty = 0; bool valid() const { return bid > 0 && ask >= bid; } };
    struct Position {
        int direction = 0; Millis entry = 0;
        double entry_perp = 0, entry_spot = 0, signal_basis = 0, entry_basis = 0, fees = 0, funding = 0;
        int funding_payments = 0;
    };
    struct Order { Millis due = 0; bool exit = false; int direction = 0; double signal_basis = 0; std::string reason; };
    struct Quote { bool active = false; double price = 0; };
    struct PendingQuote { Millis due = 0; Quote quote; };

    bool ready() const;
    double mid(Leg leg) const { return (book_[leg].bid + book_[leg].ask) / 2; }
    double perp_fee(double price, double fee_bps) const;
    void advance_to(Millis time);
    void run_due(Millis time);
    void decide();
    void decide_taker();
    void decide_maker();
    void execute(const Order& order);
    void maker_trade(double price);
    void update_center();
    double unrealized_taker() const;

    Config config_;
    double btc_; // contracts * 0.01
    Top book_[2];
    Millis now_ = -1, next_equity_ = -1, center_start_ = -1, center_time_ = -1;
    double center_ = 0;
    bool center_seeded_ = false;
    bip::FundingEstimator funding_;
    Totals realized_;
    // Taker.
    std::optional<Position> position_;
    std::optional<Order> order_;
    // Maker: [0] bid, [1] ask.
    Quote live_[2];
    std::optional<PendingQuote> pending_[2];
    int inventory_ = 0;
    double cash_ = 0;
    std::vector<RoundTrip> trips_;
    std::vector<Fill> fills_;
    std::vector<EquityPoint> equity_;
};

std::string utc(Millis ms);
// Writes trades.csv, fills.csv, equity.csv and summary.json into `directory`.
void write_results(const Engine& engine, const std::string& directory, const std::string& source);
} // namespace btc::paper
