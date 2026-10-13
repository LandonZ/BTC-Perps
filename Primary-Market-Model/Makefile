CXX = clang++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra -Wpedantic
MODEL = model/theo-model.cpp
HEADER = model/theo-model.h
BACKTEST = backtester/basis-backtest.cpp
BACKTEST_HEADER = backtester/basis-backtest.h
BACKTEST_ARGS ?=

.PHONY: all test run theo backtest clean
all: build/theo-pricer

build:
	mkdir -p build

build/theo-pricer: $(MODEL) $(HEADER) model/theo-pricer.cpp | build
	$(CXX) $(CXXFLAGS) $(MODEL) model/theo-pricer.cpp -o $@

build/theo-tests: $(MODEL) $(HEADER) tests/theo-tests.cpp | build
	$(CXX) $(CXXFLAGS) $(MODEL) tests/theo-tests.cpp -o $@

build/funding-tests: $(MODEL) $(HEADER) tests/funding-tests.cpp | build
	$(CXX) $(CXXFLAGS) $(MODEL) tests/funding-tests.cpp -o $@

build/basis-backtester: $(MODEL) $(HEADER) $(BACKTEST) $(BACKTEST_HEADER) backtester/backtest-cli.cpp | build
	$(CXX) $(CXXFLAGS) $(MODEL) $(BACKTEST) backtester/backtest-cli.cpp -o $@

build/backtest-tests: $(MODEL) $(HEADER) $(BACKTEST) $(BACKTEST_HEADER) tests/backtest-tests.cpp | build
	$(CXX) $(CXXFLAGS) $(MODEL) $(BACKTEST) tests/backtest-tests.cpp -o $@

test: build/theo-tests build/funding-tests build/backtest-tests
	./build/theo-tests
	./build/funding-tests
	./build/backtest-tests

backtest: build/basis-backtester
	./build/basis-backtester $(BACKTEST_ARGS)

theo: build/theo-pricer
	./build/theo-pricer --demo

run: theo

clean:
	rm -f build/theo-pricer build/theo-tests build/funding-tests build/basis-backtester build/backtest-tests
