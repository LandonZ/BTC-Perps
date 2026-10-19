#!/usr/bin/env python3
"""Checks tools/coinbase_basis_events.py on a tiny synthetic recording."""
import csv
import gzip
import importlib.util
import tempfile
import unittest
from pathlib import Path

TOOL = Path(__file__).resolve().parents[1] / "tools/coinbase_basis_events.py"
spec = importlib.util.spec_from_file_location("coinbase_basis_events", TOOL)
convert = importlib.util.module_from_spec(spec)
spec.loader.exec_module(convert)

HOUR = 3_600_000
T0 = 1792396800000  # 2026-10-19T08:00:00Z


class CoinbaseEvents(unittest.TestCase):
    def test_conversion(self):
        with tempfile.TemporaryDirectory() as tmp:
            day = Path(tmp)
            rows = [
                [T0 + 1000, 0, "Q", "BIP-20DEC30-CDE", 82840, 10, 82850, 5, "", "", ""],
                [T0 + 1001, 0, "Q", "BTC-USD", 82830.0, 1, 82830.01, 1, "", "", ""],
                [T0 + 1002, 0, "Q", "BTC-USD", 82830.0, 2, 82830.01, 3, "", "", ""],  # size-only change: dropped
                [T0 + 1003, 0, "T", "BIP-20DEC30-CDE", "", "", "", "", 82840, 3, "BUY"],
                [T0 + 1004, 0, "T", "BTC-USD", "", "", "", "", 82830, 0.1, "SELL"],  # spot trades unused
                [T0 + 900, 0, "T", "BIP-20DEC30-CDE", "", "", "", "", 82850, 1, "SELL"],  # clock stepped back
                [T0 + HOUR + 5, 0, "Q", "BIP-20DEC30-CDE", 82845, 1, 82855, 1, "", "", ""],
            ]
            with open(day / "events.csv", "w", newline="") as f:
                w = csv.writer(f)
                w.writerow(["recv_ms", "exch_ms", "kind", "product", "bid", "bid_qty", "ask", "ask_qty", "price", "size", "side"])
                w.writerows(rows)
            with open(day / "funding.csv", "w", newline="") as f:
                w = csv.writer(f)
                w.writerow(["recv_ms", "product", "price", "funding_rate", "funding_time", "index_price",
                            "open_interest", "future_product_details"])
                w.writerow([T0 + 500, "BIP-20DEC30-CDE", 82845, "0.000017", "2026-10-19T08:00:00Z", 82830, 1, ""])
                w.writerow([T0 + HOUR + 9000, "BIP-20DEC30-CDE", 82845, "0.000018", "2026-10-19T09:00:00Z", 82830, 1, ""])
            out = day / "basis-events.csv.gz"
            counts, estimated = convert.convert(day, out)
            with gzip.open(out, "rt") as f:
                events = list(csv.DictReader(f))
        kinds = [e["kind"] for e in events]
        self.assertEqual(kinds, ["R", "Q", "S", "T", "T", "F", "Q"])
        self.assertEqual(estimated, 0)
        r, trade, late, settle = events[0], events[3], events[4], events[5]
        self.assertEqual((int(r["time_ms"]), float(r["funding_rate"]), r["interval_hours"]), (T0, 0.000017, "1"))
        self.assertEqual(trade["buyer_maker"], "true", "Coinbase side is the maker's side")
        self.assertAlmostEqual(float(trade["quantity"]), 0.03, msg="3 contracts are 0.03 BTC")
        self.assertEqual(late["buyer_maker"], "false")
        self.assertEqual(int(late["time_ms"]), T0 + 1004, "A clock step back is held at the latest time seen")
        self.assertEqual(int(settle["time_ms"]), T0 + HOUR)
        self.assertAlmostEqual(float(settle["funding_rate"]), 0.000018, msg="The 09:00 rate settles at 09:00")
        self.assertAlmostEqual(float(settle["price"]), 82845, msg="Settlement mark is the last BIP mid")
        times = [int(e["time_ms"]) for e in events]
        self.assertEqual(times, sorted(times))


if __name__ == "__main__":
    unittest.main()
