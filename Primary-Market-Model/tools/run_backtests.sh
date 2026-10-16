#!/usr/bin/env bash
# Re-download the minute data and rerun every historical backtest:
#   1. Allen's INTX backtest as committed (reproduces results/basis-both-5m)
#   2. The same backtester on the Coinbase US nano perp (BIP) with reconstructed funding
#   3. The paper-trader engine over the same candles for INTX and BIP, without and with fees
# Needs internet (Coinbase public API). Run from anywhere:
#   bash Primary-Market-Model/tools/run_backtests.sh
# Optional environment: PYTHON (default python3), CXX (default g++), END (default today, UTC,
# exclusive), FEES (paper-trader cost flags; default is a placeholder, set your real tier).
set -euo pipefail
cd "$(dirname "$0")/.."
PY=${PYTHON:-python3}
CXX=${CXX:-g++}
START=2026-06-03
END=${END:-$(date -u +%F)}
FEES=${FEES:---perp-fee-usd 0.15 --spot-taker-bps 6}
DATA=../data/coinbase
BIP=BIP-20DEC30-CDE

echo "== Downloading minute candles (cached rows are reused)"
"$PY" tools/download_basis_data.py --start "$START" --end 2026-10-01             # INTX, paused 2026-10-01
"$PY" tools/download_basis_data.py --start "$START" --end "$END" --perp "$BIP"   # US nano perp
"$PY" tools/reconstruct_bip_funding.py

echo "== Building"
make -B build/basis-backtester build/paper-trader CXX="$CXX" >/dev/null

echo "== 1. Allen's backtest: BTC-USD vs INTX perp, recorded INTX funding, zero cost"
./build/basis-backtester --output results/basis-both-5m

echo "== 2. Same backtester on BTC-USD vs $BIP, reconstructed BIP funding, zero cost"
./build/basis-backtester --perp-csv "$DATA/${BIP}_1m.csv" \
    --funding-csv "$DATA/${BIP}_funding_1h_reconstructed.csv" --output results/bip-basis-both-5m

echo "== 3. Paper-trader engine on the same candles (taker rule, latency 0, minute opens)"
for venue in intx bip; do
    if [ "$venue" = intx ]; then
        args=(--perp-csv "$DATA/BTC-PERP-INTX_1m.csv" --funding-csv "$DATA/BTC-PERP-INTX_funding_1h.csv")
    else
        args=(--perp-csv "$DATA/${BIP}_1m.csv" --funding-csv "$DATA/${BIP}_funding_1h_reconstructed.csv")
    fi
    echo "-- $venue, zero fees"
    ./build/paper-trader --candles "${args[@]}" --output "results/candles-$venue"
    echo "-- $venue, fees: $FEES"
    # shellcheck disable=SC2086
    ./build/paper-trader --candles "${args[@]}" $FEES --output "results/candles-$venue-fees"
done

if "$PY" -c "import matplotlib" 2>/dev/null; then
    "$PY" tools/plot_backtest.py results/basis-both-5m
    "$PY" tools/plot_backtest.py results/bip-basis-both-5m
else
    echo "(matplotlib not installed: skipping plots; sudo apt install python3-matplotlib)"
fi
git checkout -- build/ 2>/dev/null || true  # Restore Allen's committed binaries; paper-trader stays.
echo "Done. Compare results/basis-both-5m, results/bip-basis-both-5m and results/candles-*/summary.json"
