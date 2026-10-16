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
MAKER = model/maker-model.cpp backtester/maker-backtest.cpp
MAKER_HEADERS = model/maker-model.h backtester/maker-backtest.h
MAKER_ARGS ?=
BASIS_MAKER = $(MAKER) backtester/basis-maker.cpp
BASIS_MAKER_HEADERS = $(MAKER_HEADERS) backtester/basis-maker.h
BASIS_MAKER_ARGS ?=

.PHONY: all test run theo backtest basis-backtest maker-backtest basis-maker paper clean
all: build/basis-maker

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

build/paper-tests: $(PAPER) $(PAPER_HEADER) $(BIP) $(BIP_HEADER) tests/paper-tests.cpp | build
	$(CXX) $(CXXFLAGS) $(PAPER) $(BIP) tests/paper-tests.cpp -o $@

build/maker-backtester: $(MAKER) $(MAKER_HEADERS) backtester/maker-backtest-cli.cpp | build
	$(CXX) $(CXXFLAGS) $(MAKER) backtester/maker-backtest-cli.cpp -o $@

build/maker-tests: $(MAKER) $(MAKER_HEADERS) tests/maker-tests.cpp | build
	$(CXX) $(CXXFLAGS) $(MAKER) tests/maker-tests.cpp -o $@

build/basis-maker: $(BASIS_MAKER) $(BASIS_MAKER_HEADERS) backtester/basis-maker-cli.cpp | build
	$(CXX) $(CXXFLAGS) $(BASIS_MAKER) backtester/basis-maker-cli.cpp -o $@

build/basis-maker-tests: $(BASIS_MAKER) $(BASIS_MAKER_HEADERS) tests/basis-maker-tests.cpp | build
	$(CXX) $(CXXFLAGS) $(BASIS_MAKER) tests/basis-maker-tests.cpp -o $@

test: build/theo-tests build/funding-tests build/bip-funding-tests build/backtest-tests build/paper-tests build/maker-tests build/basis-maker-tests
	./build/theo-tests
	./build/funding-tests
	./build/bip-funding-tests
	./build/backtest-tests
	./build/paper-tests
	./build/maker-tests
	./build/basis-maker-tests
	python3 tests/binance-data-tests.py

backtest: basis-maker

basis-maker: build/basis-maker
	./build/basis-maker $(BASIS_MAKER_ARGS)

maker-backtest: build/maker-backtester
	./build/maker-backtester $(MAKER_ARGS)

basis-backtest: build/basis-backtester
	./build/basis-backtester $(BACKTEST_ARGS)

paper: build/paper-trader
	./build/paper-trader $(PAPER_ARGS)

theo: build/theo-pricer
	./build/theo-pricer --demo

run: basis-maker

clean:
	rm -f build/theo-pricer build/theo-tests build/funding-tests build/basis-backtester build/backtest-tests build/bip-funding-tests build/paper-trader build/paper-tests build/maker-backtester build/maker-tests build/basis-maker build/basis-maker-tests
