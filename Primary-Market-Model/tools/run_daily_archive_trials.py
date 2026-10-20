#!/usr/bin/env python3
"""Run the current 2.75 bp strategy as separate day-end-liquidated UTC trials.

Pass --close-at-day-end 0 for signed unrealized marks without terminal exits.
Use separate --output and --report-dir paths to retain earlier experiments.
Arguments are forwarded to run_archive_backtest.py; the explicit daily flags
ensure inventory, quotes, event budgets and premium history reset each day.
"""
import subprocess
import sys
from pathlib import Path

if __name__=='__main__':
    model=Path(__file__).resolve().parents[1]
    raise SystemExit(subprocess.call([sys.executable,str(model/'tools/run_archive_backtest.py'),
        '--entry-offset-bps','2.75','--daily-reset','1','--close-at-day-end','1','--exit-on-opposite-signal','1',
        '--output',str(model/'build/daily-reset-2.75'),
        '--report-dir',str(model/'results/daily-reset-2.75'),*sys.argv[1:]]))
