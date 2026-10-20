#!/usr/bin/env python3
"""Run continuous inventory, then test net PnL per completed flat-to-flat trade."""
import argparse
import subprocess
import sys
from pathlib import Path

from event_trials import finalize_event_trials

MODEL = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--entry-offset-bps', type=float, default=2.75)
    parser.add_argument('--start-month', default='2023-12')
    parser.add_argument('--end-month', default='2024-02')
    parser.add_argument('--output', type=Path, default=MODEL/'build/continuous-2.75-events')
    parser.add_argument('--report-dir', type=Path, default=MODEL/'results/continuous-2.75-events')
    parser.add_argument('--analyze-only', action='store_true', help='Analyze an existing replay ledger')
    args = parser.parse_args()
    if not args.analyze_only:
        subprocess.run([sys.executable, str(MODEL/'tools/run_archive_backtest.py'),
            '--start-month', args.start_month, '--end-month', args.end_month,
            '--entry-offset-bps', str(args.entry_offset_bps), '--daily-reset', '0',
            '--close-at-day-end', '0', '--exit-on-opposite-signal', '1',
            '--output', str(args.output), '--report-dir', str(args.report_dir)], check=True)
    finalize_event_trials(args.output, args.report_dir)


if __name__ == '__main__':
    main()
