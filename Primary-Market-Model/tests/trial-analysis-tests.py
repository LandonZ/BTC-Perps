#!/usr/bin/env python3
"""Check the statistical test direction, serial dependence and resampling."""
import math
import csv
import json
import re
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
from run_trials import analyze, make_report
from run_archive_backtest import continuous_statistics, verify_sides, verify_funding
from run_backtest import render_report


class TrialAnalysisTests(unittest.TestCase):
    def test_known_t_statistic_and_one_sided_direction(self):
        positive = analyze([1,2,3,4,5], bootstrap_samples=2000)
        negative = analyze([-1,-2,-3,-4,-5], bootstrap_samples=2000)
        self.assertAlmostEqual(positive['t_test']['statistic'], 3/math.sqrt(.5))
        self.assertAlmostEqual(positive['t_test']['p_one_sided'], .006617799781841345)
        self.assertAlmostEqual(positive['t_test']['p_one_sided']+negative['t_test']['p_one_sided'], 1)
        centered = analyze(list(range(-15,16)), bootstrap_samples=2000)
        self.assertEqual(centered['t_test']['p_one_sided'], .5)

    def test_hac_against_bartlett_kernel_matrix(self):
        import numpy as np
        x = np.arange(-15,16,dtype=float)
        result = analyze(x, bootstrap_samples=2000)
        for lag in (3,7):
            distance = np.abs(np.arange(len(x))[:,None]-np.arange(len(x))[None,:])
            kernel = np.maximum(0,1-distance/(lag+1))
            expected = math.sqrt(float(x @ kernel @ x)/(len(x)*(len(x)-1)))
            self.assertAlmostEqual(result['hac'][str(lag)]['standard_error'], expected)
            self.assertGreater(expected,result['t_test']['standard_error'])

    def test_bootstrap_reproducible_and_scale_invariant(self):
        x = [-4,3,2,0,-1,5,8,-2,2,1]
        a = analyze(x, bootstrap_samples=2000)
        b = analyze(x, bootstrap_samples=2000)
        scaled = analyze([v*10 for v in x], bootstrap_samples=2000)
        self.assertEqual(a,b)
        for length in ('3','7'):
            self.assertEqual(a['block_bootstrap'][length]['p_one_sided_centered'],
                             scaled['block_bootstrap'][length]['p_one_sided_centered'])
            for first,second in zip(a['block_bootstrap'][length]['ci95_basic'],
                                    scaled['block_bootstrap'][length]['ci95_basic']):
                self.assertAlmostEqual(first*10,second)

    def test_invalid_samples_fail(self):
        for x in ([1,2], [2,2,2], [1,float('nan'),3]):
            with self.assertRaises(ValueError):
                analyze(x)

    def test_duplicate_or_gapped_days_are_not_additional_trials(self):
        for days in (['2023-07-01','2023-07-01','2023-07-02'],
                     ['2023-07-01','2023-07-03','2023-07-04']):
            with self.assertRaises(ValueError):
                make_report([dict(date=day) for day in days])

    def test_rendering_cannot_silently_ignore_a_fee_override(self):
        script=Path(__file__).resolve().parents[1]/'tools/run_trials.py'
        for key in ('--spot-taker-bps','--entry-offset-bps'):
            result=subprocess.run([sys.executable,str(script),'--from-results','unused.json',
                                   key,'0.5'],capture_output=True,text=True)
            self.assertEqual(result.returncode,2)
            self.assertIn('require a replay',result.stderr)

    def test_continuous_daily_changes_reconcile_without_inventory_resets(self):
        with tempfile.TemporaryDirectory(prefix='continuous-stat-test-') as temporary:
            directory=Path(temporary)
            with (directory/'equity.csv').open('w',newline='') as handle:
                writer=csv.writer(handle)
                writer.writerow(['time_ms','net_pnl','price_pnl','fees','funding_pnl'])
                writer.writerows([[1701475199999,10,12,2,0],[1701561599999,25,27,3,1],[1701647999999,5,7,4,2]])
            summary=dict(net_pnl=5,perp_maker_bps=0,perp_taker_bps=.5,spot_taker_bps=.5,
                         equilibrium_bps=-5,entry_bid_basis_bps=-6.5,entry_ask_basis_bps=-3.5)
            result=continuous_statistics(directory,summary)
            self.assertTrue(result['continuous_inventory'])
            self.assertEqual([t['summary']['net_pnl'] for t in result['trials']],[10,15,-20])
            self.assertEqual(result['statistics']['total'],5)
            saved=directory/'saved.json'
            saved.write_text(json.dumps(result))
            output=directory/'redrawn.json'
            subprocess.run([sys.executable,str(Path(__file__).resolve().parents[1]/'tools/run_trials.py'),
                            '--from-results',str(saved),'--output',str(output)],check=True,capture_output=True)
            self.assertTrue(json.loads(output.read_text())['continuous_inventory'])

    def test_direction_report_keeps_closed_pair_gains_and_excludes_funding_from_price(self):
        with tempfile.TemporaryDirectory(prefix='direction-report-test-') as temporary:
            directory=Path(temporary)
            names=('long_perp_short_spot','short_perp_long_spot')
            # At the second mark both directions are open; at the final mark
            # the profitable long pair is closed and only the short remains.
            states=[[(0,0,0,0,0),(0,0,0,0,0)],
                    [(7,0,-1,1,.004),(-4,0,5,1,.004)],
                    [(0,9,-1,2,0),(-6,0,6,1,.004)]]
            records=[]
            for index,state in enumerate(states):
                row={'time_ms':1701388800000+index*60000}
                for name,(unrealized,realized,funding,fees,btc) in zip(names,state):
                    row.update({name+'_'+key:value for key,value in dict(unrealized_pnl=unrealized,
                        realized_price_pnl=realized,funding_pnl=funding,fees=fees,open_btc=btc,
                        net_pnl=unrealized+realized+funding).items()})
                for key in ('net_pnl','fees','funding_pnl','unrealized_pnl'):
                    row[key]=sum(row[name+'_'+key] for name in names)
                row['realized_pnl']=sum(row[name+'_realized_price_pnl'] for name in names)+row['funding_pnl']
                row['price_pnl']=row['net_pnl']+row['fees']-row['funding_pnl']
                row['open_gross_perp_btc']=sum(row[name+'_open_btc'] for name in names)
                records.append(row)
            summary=dict(net_pnl=records[-1]['net_pnl'],basis_sides={name:{
                key:records[-1][name+'_'+key] for key in ('unrealized_pnl','realized_price_pnl','funding_pnl','fees','open_btc','net_pnl')}
                for name in names})
            (directory/'summary.json').write_text(json.dumps(summary))
            def save():
                with (directory/'equity.csv').open('w',newline='') as handle:
                    writer=csv.DictWriter(handle,fieldnames=records[0]);writer.writeheader();writer.writerows(records)
            save()
            validation=verify_sides(directory,summary)
            self.assertEqual(validation['side_attribution_marks'],3)
            self.assertEqual(max(validation['max_side_attribution_errors'].values()),0)
            render_report(directory,directory/'result.html')
            html=(directory/'result.html').read_text()
            data=json.loads(re.search(r'<script id="results" type="application/json">(.*?)</script>',html,re.S).group(1))
            self.assertEqual(data['points'][-1][3:],[0,-6,9,-6])
            # Funding mistakenly included in unrealized must fail even when
            # it leaves the saved total-profit column unchanged.
            records[1][names[0]+'_unrealized_pnl']+=records[1][names[0]+'_funding_pnl']
            save()
            with self.assertRaisesRegex(ValueError,'unrealized_pnl attribution failed'):
                verify_sides(directory,summary)

    def test_funding_audit_uses_exact_inventory_and_reports_residual_payments(self):
        with tempfile.TemporaryDirectory(prefix='funding-audit-test-') as temporary:
            directory=Path(temporary);first=1701417600000;second=first+8*3600000
            sources={}
            for name,records in [('mark',[[first,100000],[second,100000]]),
                                 ('funding',[[first,8,.0001],[second,8,.0001]])]:
                path=directory/(name+'.zip')
                with zipfile.ZipFile(path,'w') as archive:
                    archive.writestr(name+'.csv','\n'.join(','.join(map(str,row)) for row in records)+'\n')
                sources[name]={'path':str(path)}
            records=[]
            # Long exits after the coarse minute mark. Exact pre-settlement
            # inventory must show only the short, which receives funding.
            # At the second settlement a stranded long pays: the audit must
            # disclose that payment even though cumulative net funding is zero.
            for time,long_btc,short_btc,long_funding,short_funding in [
                (first-60000,.004,.004,0,0),(first-1000,0,.004,0,0),
                (first,0,.004,0,.04),(second-1000,.004,0,0,.04),
                (second,.004,0,-.04,.04)]:
                records.append(dict(time_ms=time,perp_btc=long_btc-short_btc,
                    long_perp_short_spot_open_btc=long_btc,short_perp_long_spot_open_btc=short_btc,
                    long_perp_short_spot_funding_pnl=long_funding,short_perp_long_spot_funding_pnl=short_funding,
                    funding_pnl=long_funding+short_funding))
            with (directory/'equity.csv').open('w',newline='') as handle:
                writer=csv.DictWriter(handle,fieldnames=records[0]);writer.writeheader();writer.writerows(records)
            audit=verify_funding(directory,[{'sources':sources}])
            self.assertEqual(audit['funding_settlements'],2)
            self.assertAlmostEqual(audit['gross_funding_paid_usdt'],.04)
            self.assertAlmostEqual(audit['gross_funding_received_usdt'],.04)
            self.assertEqual(len(audit['paying_side_settlements']),1)
            self.assertEqual(audit['paying_side_settlements'][0]['time_ms'],second)
            self.assertEqual(audit['paying_side_settlements'][0]['side'],'long_perp_short_spot')


if __name__ == '__main__':
    unittest.main()
