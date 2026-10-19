CXX = clang++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra -Wpedantic
MODEL = model/theo-model.cpp
HEADER = model/theo-model.h
BACKTEST = backtester/basis-backtest.cpp
BACKTEST_HEADER = backtester/basis-backtest.h
BACKTEST_ARGS ?=
BIP = model/bip-funding.cpp
BIP_HEADER = model/bip-funding.h
PAPER = paper/paper-engine.cpp
PAPER_HEADER = paper/paper-engine.h
PAPER_ARGS ?= --help

.PHONY: all test run theo backtest paper clean
all: build/theo-pricer

build:
	mkdir -p build

build/theo-pricer: $(MODEL) $(HEADER) model/theo-pricer.cpp | build
	$(CXX) $(CXXFLAGS) $(MODEL) model/theo-pricer.cpp -o $@

build/theo-tests: $(MODEL) $(HEADER) tests/theo-tests.cpp | build
	$(CXX) $(CXXFLAGS) $(MODEL) tests/theo-tests.cpp -o $@

build/funding-tests: $(MODEL) $(HEADER) tests/funding-tests.cpp | build
	$(CXX) $(CXXFLAGS) $(MODEL) tests/funding-tests.cpp -o $@

build/bip-funding-tests: $(BIP) $(BIP_HEADER) $(MODEL) $(HEADER) tests/bip-funding-tests.cpp | build
	$(CXX) $(CXXFLAGS) $(BIP) $(MODEL) tests/bip-funding-tests.cpp -o $@

build/basis-backtester: $(MODEL) $(HEADER) $(BACKTEST) $(BACKTEST_HEADER) backtester/backtest-cli.cpp | build
	$(CXX) $(CXXFLAGS) $(MODEL) $(BACKTEST) backtester/backtest-cli.cpp -o $@

build/backtest-tests: $(MODEL) $(HEADER) $(BACKTEST) $(BACKTEST_HEADER) tests/backtest-tests.cpp | build
	$(CXX) $(CXXFLAGS) $(MODEL) $(BACKTEST) tests/backtest-tests.cpp -o $@

build/paper-trader: $(PAPER) $(PAPER_HEADER) $(BIP) $(BIP_HEADER) $(MODEL) $(HEADER) $(BACKTEST) $(BACKTEST_HEADER) paper/paper-cli.cpp | build
	$(CXX) $(CXXFLAGS) $(PAPER) $(BIP) $(MODEL) $(BACKTEST) paper/paper-cli.cpp -o $@

build/paper-tests: $(PAPER) $(PAPER_HEADER) $(BIP) $(BIP_HEADER) $(MODEL) $(HEADER) tests/paper-tests.cpp | build
	$(CXX) $(CXXFLAGS) $(PAPER) $(BIP) $(MODEL) tests/paper-tests.cpp -o $@

test: build/theo-tests build/funding-tests build/bip-funding-tests build/backtest-tests build/paper-tests
	./build/theo-tests
	./build/funding-tests
	./build/bip-funding-tests
	./build/backtest-tests
	./build/paper-tests

backtest: build/basis-backtester
	./build/basis-backtester $(BACKTEST_ARGS)

paper: build/paper-trader
	./build/paper-trader $(PAPER_ARGS)

theo: build/theo-pricer
	./build/theo-pricer --demo

run: theo

clean:
	rm -f build/theo-pricer build/theo-tests build/funding-tests build/basis-backtester build/backtest-tests build/bip-funding-tests build/paper-trader build/paper-tests
