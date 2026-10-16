CXX = clang++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra -Wpedantic
SOURCES = backtester/events.cpp backtester/basis-maker.cpp
HEADERS = backtester/events.h backtester/basis-maker.h
ARGS ?=
MONTH ?= 2023-07

.PHONY: all run backtest basis-maker sample month test clean
all: build/basis-maker

build:
	mkdir -p build

build/basis-maker: $(SOURCES) $(HEADERS) backtester/basis-maker-cli.cpp | build
	$(CXX) $(CXXFLAGS) $(SOURCES) backtester/basis-maker-cli.cpp -o $@

build/basis-maker-tests: $(SOURCES) $(HEADERS) tests/basis-maker-tests.cpp | build
	$(CXX) $(CXXFLAGS) $(SOURCES) tests/basis-maker-tests.cpp -o $@

run backtest basis-maker: build/basis-maker
	python3 tools/run_backtest.py $(ARGS)

sample:
	python3 tools/download_binance_maker.py

month: build/basis-maker
	python3 tools/prepare_binance_basis_month.py --month $(MONTH)
	python3 tools/run_backtest.py --events-list ../data/binance/maker/$(MONTH)/events-list.txt $(ARGS)

test: build/basis-maker-tests
	./build/basis-maker-tests
	python3 tests/binance-data-tests.py

clean:
	rm -rf build
