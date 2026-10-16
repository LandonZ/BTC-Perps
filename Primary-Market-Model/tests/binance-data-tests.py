#!/usr/bin/env python3
"""Checks for causal Binance archive conversion, without network calls."""
import csv
import importlib.util
import io
import tempfile
import unittest
import zipfile
from pathlib import Path

path = Path(__file__).resolve().parents[1] / 'tools/download_binance_maker.py'
spec = importlib.util.spec_from_file_location('download_binance_maker', path)
download = importlib.util.module_from_spec(spec)
spec.loader.exec_module(download)


def archive(path, rows):
    text = io.StringIO()
    csv.writer(text).writerows(rows)
    with zipfile.ZipFile(path, 'w') as handle:
        handle.writestr('data.csv', text.getvalue())


class BinanceDataTests(unittest.TestCase):
    def test_external_sort_preserves_priority_and_ties_across_merge_passes(self):
        events = [download.event(1000+i%3,kind,price=str(i))
                  for i,kind in enumerate(['T','Q','S','F','R','T','Q','T','S','Q','T','S','T','Q'])]
        priority = {'R': 0, 'F': 0, 'S': 1, 'Q': 2, 'T': 3}
        expected = sorted(events,key=lambda row:(int(row[0]),priority[row[1]]))
        with tempfile.TemporaryDirectory(prefix='binance-sort-test-') as temporary:
            actual = list(download.sorted_events(iter(events),Path(temporary),chunk_rows=2,fan_in=2))
        self.assertEqual(actual,[[str(cell) for cell in row] for row in expected])

    def test_causal_stream_and_funding_availability(self):
        start = 1689811200000
        with tempfile.TemporaryDirectory(prefix='binance-data-test-') as temporary:
            root = Path(temporary)
            paths = {key: root / f'{key}.zip' for key in ['book', 'trades', 'spot', 'mark', 'funding']}
            archive(paths['book'], [
                ['update_id', 'bid', 'bid_qty', 'ask', 'ask_qty', 'transaction_time', 'event_time'],
                [1, 30000, 1, 30000.1, 1, start+60000, start+61000],
                [2, 30001, 1, 30001.1, 1, start+1, start+2], # deliberately out of order
            ])
            archive(paths['trades'], [
                ['agg_trade_id', 'price', 'quantity', 'first', 'last', 'transact_time', 'is_buyer_maker'],
                [1, 30000, 0.01, 1, 1, start+60000, 'true'],
            ])
            archive(paths['spot'], [
                [start*1000, 1, 999999, 1, 29999, 1, (start+59999)*1000+999],
                [(start+60000)*1000, 1, 999999, 1, 88888, 1, (start+119999)*1000+999],
            ])
            archive(paths['mark'], [[start, 30002], [start+60000, 30003]])
            archive(paths['funding'], [
                ['calc_time', 'funding_interval_hours', 'last_funding_rate'],
                [start-8*3600000, 8, 0.00001],
                [start, 8, 0.00002],
                [start+60001, 1, 0.00099], # millisecond offset still finds mark minute
            ])
            output = root / 'events.csv'
            download.prepare(paths, start, start+120000, output)
            with output.open() as handle:
                events = list(csv.DictReader(handle))
            self.assertEqual([int(r['time_ms']) for r in events], sorted(int(r['time_ms']) for r in events))
            spots = [r for r in events if r['kind'] == 'S']
            self.assertEqual(len(spots), 1)
            self.assertEqual(int(spots[0]['time_ms']), start+60000)
            self.assertEqual(float(spots[0]['price']), 29999)
            self.assertFalse(any(r['price'] == '88888' for r in events))
            # Funding never moves to an earlier availability time.
            later = [r for r in events if r['funding_rate'] == '0.00099']
            self.assertEqual(len(later), 1)
            self.assertEqual(int(later[0]['time_ms']), start+60001)
            self.assertEqual(float(later[0]['price']), 30003)
            self.assertEqual(events[0]['kind'], 'R')
            self.assertEqual(int(events[0]['time_ms']), start-8*3600000)
            same_time = [r['kind'] for r in events if int(r['time_ms']) == start+60000]
            self.assertEqual(same_time, ['S', 'Q', 'T'])
            self.assertEqual(int(next(r['time_ms'] for r in events if r['kind'] == 'Q')), start+1)

            # Tick spot trades replace the candle proxy without leaking future prints.
            paths['spot_trades'] = root / 'spot_trades.zip'
            archive(paths['spot_trades'], [
                [1, 29990, 0.01, 1, 1, start-5, 'true', 'true'],
                [2, 29991, 0.01, 2, 2, start+10, 'false', 'true'],
                [3, 99999, 0.01, 3, 3, start+120000, 'false', 'true'],
            ])
            download.prepare(paths,start,start+120000,output)
            with output.open() as handle:
                spot_events = [r for r in csv.DictReader(handle) if r['kind'] == 'S']
            self.assertEqual([int(r['time_ms']) for r in spot_events], [start-5,start+10])
            self.assertEqual([r['price'] for r in spot_events], ['29990','29991'])

    def test_missing_feed_fails_instead_of_fabricating_orders(self):
        with tempfile.TemporaryDirectory(prefix='binance-missing-test-') as temporary:
            root = Path(temporary)
            paths = {key: root / f'{key}.zip' for key in ['book', 'trades', 'spot', 'mark', 'funding']}
            for path in paths.values():
                archive(path, [])
            with self.assertRaises(ValueError):
                download.prepare(paths, 0, 100000, root / 'events.csv')


if __name__ == '__main__':
    unittest.main()
