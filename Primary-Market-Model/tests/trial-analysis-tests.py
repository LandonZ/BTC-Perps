#!/usr/bin/env python3
"""Check the statistical test direction, serial dependence and resampling."""
import math
import subprocess
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
from run_trials import analyze, make_report


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


if __name__ == '__main__':
    unittest.main()
