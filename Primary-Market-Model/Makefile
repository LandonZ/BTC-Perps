CXX = clang++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra -Wpedantic
SOURCES = backtester/events.cpp backtester/basis-maker.cpp
HEADERS = backtester/events.h backtester/basis-maker.h
ARGS ?=
MONTH ?= 2024-02

.PHONY: all run backtest daily-trials event-trials basis-maker month test clean
all: build/basis-maker

build:
	mkdir -p build

build/basis-maker: $(SOURCES) $(HEADERS) backtester/basis-maker-cli.cpp | build
	$(CXX) $(CXXFLAGS) $(SOURCES) backtester/basis-maker-cli.cpp -o $@

build/basis-maker-tests: $(SOURCES) $(HEADERS) tests/basis-maker-tests.cpp | build
	$(CXX) $(CXXFLAGS) $(SOURCES) tests/basis-maker-tests.cpp -o $@

build/archive-replay: $(SOURCES) $(HEADERS) backtester/archive-format.h backtester/archive-replay.cpp | build
	$(CXX) $(CXXFLAGS) $(SOURCES) backtester/archive-replay.cpp -o $@

build/archive-sort: backtester/archive-sort.cpp backtester/archive-format.h backtester/events.h | build
	$(CXX) $(CXXFLAGS) backtester/archive-sort.cpp -o $@

run backtest: build/archive-replay
	build/trial-venv/bin/python tools/run_archive_backtest.py $(ARGS)

daily-trials: build/archive-replay
	build/trial-venv/bin/python tools/run_daily_archive_trials.py $(ARGS)

event-trials: build/archive-replay
	build/trial-venv/bin/python tools/run_continuous_event_trials.py $(ARGS)

basis-maker: build/basis-maker
	python3 tools/run_backtest.py $(ARGS)

month: build/archive-replay
	python3 tools/prepare_binance_basis_month.py --month $(MONTH) --archives-only
	build/trial-venv/bin/python tools/run_archive_backtest.py --start-month $(MONTH) --end-month $(MONTH) $(ARGS)

test: build/basis-maker-tests build/basis-maker build/archive-replay build/archive-sort
	./build/basis-maker-tests
	python3 tests/binance-data-tests.py
	python3 tests/archive-replay-tests.py

clean:
	rm -rf build
