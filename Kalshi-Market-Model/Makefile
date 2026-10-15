CXX = clang++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra -Wpedantic
BACKTEST_ARGS ?=
BACKTESTER = ./build/kalshi-backtester
PLOT = python3 tools/plot_backtest.py

.PHONY: all help live backtest backtest-touch backtest-settled report sweep-exit sweep-hold sweeps maker maker-report everything clean
all: build/kalshi-backtester build/kalshi-maker build/kalshi-live

help:
	@echo "make                  build the backtester and maker simulator"
	@echo "make report           rerun the 4 headline backtests and regenerate their charts/reports"
	@echo "make sweep-exit       rerun results/exit1-sweep (entry threshold x profit exit x fill)"
	@echo "make sweep-hold       rerun results/exit1-hold (touch fills, varying max hold)"
	@echo "make sweeps           both sweeps"
	@echo "make maker-report     rerun results/maker (maker simulation on tick data)"
	@echo "make maker            one maker run; pass options with BACKTEST_ARGS=\"--edge-bps 3 ...\""
	@echo "make live             paper-trade the naive model on live Kalshi + Coinbase quotes; options via LIVE_ARGS"
	@echo "make everything       report + both sweeps + maker-report"
	@echo "make backtest         one run; pass options with BACKTEST_ARGS=\"--threshold-bps 3 ...\""

build:
	mkdir -p build

build/kalshi-backtester: backtester/kalshi-backtester.cpp | build
	$(CXX) $(CXXFLAGS) $< -o $@

backtest: build/kalshi-backtester
	$(BACKTESTER) $(BACKTEST_ARGS)

backtest-touch: build/kalshi-backtester
	$(BACKTESTER) --fill touch --output results/basis-both-5m-touch $(BACKTEST_ARGS)

# Same runs from 2026-06-09, after the thin launch-week Kalshi book.
backtest-settled: build/kalshi-backtester
	$(BACKTESTER) --start 2026-06-09 --output results/basis-both-5m-from-jun09 $(BACKTEST_ARGS)
	$(BACKTESTER) --start 2026-06-09 --fill touch --output results/basis-both-5m-touch-from-jun09 $(BACKTEST_ARGS)

report: backtest backtest-touch backtest-settled
	for d in results/basis-both-5m*; do $(PLOT) $$d; done

# Entry threshold x profit exit x fill; 5-minute hold, exit at 1 bps, from 2026-06-09.
sweep-exit: build/kalshi-backtester
	for fill in mid touch; do for entry in 2 3 5 8; do for pe in 0 1; do \
	  d=results/exit1-sweep/$$fill-entry$$entry-profitexit$$pe; \
	  $(BACKTESTER) --start 2026-06-09 --fill $$fill --threshold-bps $$entry \
	    --exit-bps 1 --profit-exit $$pe --output $$d > /dev/null && $(PLOT) $$d || exit 1; \
	done; done; done

# Touch fills, no profit exit, exit at 1 bps; varying the maximum hold.
sweep-hold: build/kalshi-backtester
	for entry in 3 5; do for hold in 5 15 30 60 240; do \
	  $(BACKTESTER) --start 2026-06-09 --fill touch --threshold-bps $$entry \
	    --exit-bps 1 --profit-exit 0 --hold-minutes $$hold \
	    --output results/exit1-hold/touch-entry$$entry-hold$$hold > /dev/null || exit 1; \
	done; done

sweeps: sweep-exit sweep-hold

# ---------------------------------------------------------------- maker simulation
# Needs tick data: python3 ../data/fetch_data.py kalshi and coinbase-ticks.
MAKER = ./build/kalshi-maker
MAKER_DIR = results/maker
MAKER_BASE = --center-minutes 60 --edge-bps 2

build/kalshi-maker: backtester/kalshi-maker.cpp | build
	$(CXX) $(CXXFLAGS) $< -o $@

maker: build/kalshi-maker
	$(MAKER) $(BACKTEST_ARGS)

# Each run changes one thing from MAKER_BASE (trailing 60-min fair value, +/-2 bps,
# 1 s quote and hedge latency, back of queue, no fees).
maker-report: build/kalshi-maker
	$(MAKER) --center-bps 0 --edge-bps 2 --output $(MAKER_DIR)/fair-at-spot > /dev/null
	for e in 1 2 3 5; do $(MAKER) --center-minutes 60 --edge-bps $$e --output $(MAKER_DIR)/edge$$e > /dev/null || exit 1; done
	for l in 0 0.5 2; do $(MAKER) $(MAKER_BASE) --quote-latency $$l --hedge-latency $$l --output $(MAKER_DIR)/edge2-latency$$l > /dev/null || exit 1; done
	$(MAKER) $(MAKER_BASE) --queue front --output $(MAKER_DIR)/edge2-queue-front > /dev/null
	# Best case (no latency) with fees: Kalshi top tier + Coinbase top tier, then both base tiers.
	$(MAKER) $(MAKER_BASE) --quote-latency 0 --hedge-latency 0 --perp-maker-fee-bps 0.6 --perp-taker-fee-bps 2.6 \
	  --spot-fee-bps 4 --output $(MAKER_DIR)/edge2-latency0-fees-top-tier > /dev/null
	$(MAKER) $(MAKER_BASE) --quote-latency 0 --hedge-latency 0 --perp-maker-fee-bps 5 --perp-taker-fee-bps 12 \
	  --spot-fee-bps 60 --output $(MAKER_DIR)/edge2-latency0-fees-base-tier > /dev/null
	python3 tools/maker_report.py $(MAKER_DIR)

# ---------------------------------------------------------------- live paper trading
# Naive model (model/) on live quotes; places no orders. Logs to live/logs/.
LIVE_ARGS ?=

build/kalshi-live: live/kalshi-live.cpp model/kalshi-model.cpp model/kalshi-model.hpp | build
	$(CXX) $(CXXFLAGS) live/kalshi-live.cpp model/kalshi-model.cpp -lcurl -o $@

live: build/kalshi-live
	./build/kalshi-live $(LIVE_ARGS)

everything: report sweeps maker-report

clean:
	rm -f build/kalshi-backtester build/kalshi-maker build/kalshi-live
