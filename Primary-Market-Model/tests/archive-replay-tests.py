#!/usr/bin/env python3
"""Compare streaming archives with the original normalized-CSV replay."""
import csv
import json
import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

MODEL=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(MODEL/'tools'))
from download_binance_maker import prepare, event, COLUMNS

START=1701388800000
DAY=86400000


def write_csv(path, data):
    with path.open('w',newline='') as handle: csv.writer(handle).writerows(data)


class ArchiveReplayTests(unittest.TestCase):
    def test_matches_csv_engine_with_disorder_ties_funding_and_carried_inventory(self):
        with tempfile.TemporaryDirectory(prefix='archive-replay-test-') as temporary:
            root=Path(temporary)
            data={key:[] for key in ['book','trades','spot_trades','mark','funding']}
            for day in range(3):
                t=START+day*DAY
                for hour in (0,8,16):
                    f=t+hour*3600000
                    data['mark'].append([f,100000+day*100])
                    data['funding'].append([f+(1 if hour==8 else 0),8,.0003])
                for offset in (10,8*3600000-61000,DAY-1000):
                    u=t+offset
                    data['spot_trades'].extend([[1,100000+day*100,1,1,1,u,'true','true'],
                                                [2,100000+day*100,1,2,2,u+100,'false','true']])
                    data['book'].extend([[1,99930+day*100,1,99940+day*100,1,u+2,u+2],
                                         [2,99930+day*100,1,99940+day*100,1,u+1,u+1],
                                         [3,99960+day*100,1,99970+day*100,1,u+6,u+6]])
                    data['trades'].extend([[1,99930+day*100,.04,1,1,u+5,'true'],
                                           [2,99970+day*100,.04,2,2,u+7,'false']])
            paths={}
            raw={}
            # Monthly Binance books can jump across most of a month, rather
            # than merely arriving a few milliseconds out of order.
            data['book']=data['book'][:1]+data['book'][-1:]+data['book'][1:-1]
            for name,records in data.items():
                raw[name]=root/(name+'.csv');write_csv(raw[name],records)
                paths[name]=root/(name+'.zip')
                with zipfile.ZipFile(paths[name],'w') as archive: archive.write(raw[name],name+'.csv')
            prepare(paths,START,START+3*DAY,root/'events.csv')
            premium=[event(t,'P',.002) for t in range(START+60000,START+3*DAY,60000)]
            write_csv(root/'premium.csv',[COLUMNS,*premium])
            subprocess.run([str(MODEL/'build/basis-maker'),'--events',str(root/'events.csv'),
                            '--premium-events',str(root/'premium.csv'),'--details','1','--output',str(root/'reference')],
                           check=True,capture_output=True)
            small=[event(START-8*3600000,'R',rate=0,interval=8)]+premium+[event(int(row[0]),'F',100000+(int(row[0])-START)//DAY*100,
                                 rate=row[2],interval=row[1]) for row in data['funding']]
            small.sort(key=lambda r:(int(r[0]),r[1]!='F'))
            write_csv(root/'small.csv',[COLUMNS,*small])
            with (root/'sources.tsv').open('w') as manifest:
                for kind,name in [('Q','book'),('T','trades'),('S','spot_trades')]:
                    binary=root/(name+'.bin')
                    with raw[name].open('rb') as source,binary.open('wb') as destination:
                        subprocess.run([str(MODEL/'build/archive-sort'),kind,str(root/('runs-'+name)),str(root),'3'],
                                       stdin=source,stdout=destination,stderr=subprocess.PIPE,check=True)
                    manifest.write(f'{kind} {START} {START+3*DAY} {binary} BINARY\n')
            subprocess.run([str(MODEL/'build/archive-replay'),str(root/'sources.tsv'),str(root/'small.csv'),
                            str(root/'streaming')],check=True,capture_output=True)
            reference=json.loads((root/'reference/summary.json').read_text())
            actual=json.loads((root/'streaming/summary.json').read_text())
            self.assertEqual(reference,actual)
            with (root/'streaming/equity.csv').open() as handle: equity=list(csv.DictReader(handle))
            daily={int(row['time_ms'])//DAY:row for row in equity}
            self.assertEqual(len(daily),3)
            self.assertEqual(min(daily),START//DAY,'Settlement audit must not add the pre-window schedule seed as another equity day')
            self.assertEqual(int(daily[START//DAY]['time_ms']),START+DAY-900)
            self.assertGreater(actual['paired_partial_fills'],0)
            self.assertGreater(actual['funding_exit_pairs'],0)
            subprocess.run([str(MODEL/'build/basis-maker'),'--events',str(root/'events.csv'),
                            '--premium-events',str(root/'premium.csv'),'--equilibrium-bps','0',
                            '--entry-offset-bps','1.5','--output',str(root/'zero-reference')],
                           check=True,capture_output=True)
            subprocess.run([str(MODEL/'build/archive-replay'),str(root/'sources.tsv'),str(root/'small.csv'),
                            str(root/'zero-streaming'),'--equilibrium-bps','0','--entry-offset-bps','1.5'],
                           check=True,capture_output=True)
            zero=json.loads((root/'zero-streaming/summary.json').read_text())
            self.assertEqual(zero,json.loads((root/'zero-reference/summary.json').read_text()))
            self.assertEqual((zero['equilibrium_bps'],zero['entry_bid_basis_bps'],zero['entry_ask_basis_bps']),(0,-1.5,1.5))
            self.assertEqual(zero['entry_band_violations'],0)
            self.assertFalse(zero['funding_exit_flat_region_exempt'])
            settings=['--equilibrium-bps','-5','--entry-offset-bps','1.25',
                      '--perp-maker-bps','0.5','--perp-taker-bps','2','--spot-taker-bps','2',
                      '--one-order-per-entry-event','1']
            subprocess.run([str(MODEL/'build/basis-maker'),'--events',str(root/'events.csv'),
                            '--premium-events',str(root/'premium.csv'),*settings,
                            '--output',str(root/'event-reference')],check=True,capture_output=True)
            subprocess.run([str(MODEL/'build/archive-replay'),str(root/'sources.tsv'),str(root/'small.csv'),
                            str(root/'event-streaming'),*settings],check=True,capture_output=True)
            final=json.loads((root/'event-streaming/summary.json').read_text())
            self.assertEqual(final,json.loads((root/'event-reference/summary.json').read_text()))
            self.assertEqual((final['entry_bid_basis_bps'],final['entry_ask_basis_bps']),(-6.25,-3.75))
            self.assertEqual((final['perp_maker_bps'],final['perp_taker_bps'],final['spot_taker_bps']),(.5,2,2))
            self.assertTrue(final['one_order_per_entry_event'])
            self.assertLessEqual(final['max_entry_event_btc'],final['order_btc'])
            limited_settings=[*settings,'--single-basis-position','1']
            subprocess.run([str(MODEL/'build/basis-maker'),'--events',str(root/'events.csv'),
                            '--premium-events',str(root/'premium.csv'),*limited_settings,
                            '--output',str(root/'limited-reference')],check=True,capture_output=True)
            subprocess.run([str(MODEL/'build/archive-replay'),str(root/'sources.tsv'),str(root/'small.csv'),
                            str(root/'limited-streaming'),*limited_settings],check=True,capture_output=True)
            limited=json.loads((root/'limited-streaming/summary.json').read_text())
            self.assertEqual(limited,json.loads((root/'limited-reference/summary.json').read_text()))
            self.assertTrue(limited['single_basis_position'])
            self.assertLessEqual(limited['max_open_gross_perp_btc'],.01+1e-9)

    def test_multi_pass_sort_preserves_timestamp_ties_and_every_record(self):
        with tempfile.TemporaryDirectory(prefix='archive-sort-test-') as temporary:
            root=Path(temporary)
            data=[[i,100+i,.01,0,0,START+i%17,'true' if i%2 else 'false'] for i in range(310)]
            write_csv(root/'raw.csv',data)
            with (root/'raw.csv').open('rb') as source,(root/'sorted.bin').open('wb') as output:
                subprocess.run([str(MODEL/'build/archive-sort'),'T',str(root/'runs'),str(root),'1'],
                               stdin=source,stdout=output,stderr=subprocess.PIPE,check=True)
            raw=(root/'sorted.bin').read_bytes()
            self.assertEqual(raw[:16],b'BASIS-ARCHIVE-1\n')
            records=list(struct.iter_unpack('=qddQ',raw[16:]))
            expected=sorted([(row[5],float(row[1]),row[2]*(1 if i%2 else -1),i) for i,row in enumerate(data)])
            self.assertEqual(records,expected)
            self.assertFalse(list((root/'runs').glob('*.bin')))

    def test_excessive_disorder_fails_without_publishing_results(self):
        with tempfile.TemporaryDirectory(prefix='archive-disorder-test-') as temporary:
            root=Path(temporary)
            write_csv(root/'book.csv',[[1,99930,1,99940,1,START+offset,START+offset]
                                     for offset in (1000,4000,5000,0)])
            (root/'sources.tsv').write_text(f'Q {START} {START+DAY} {root/"book.csv"}\n')
            write_csv(root/'small.csv',[COLUMNS,event(START,'F',100000,rate=.0001,interval=8)])
            result=subprocess.run([str(MODEL/'build/archive-replay'),str(root/'sources.tsv'),
                                   str(root/'small.csv'),str(root/'output')],capture_output=True,text=True)
            self.assertNotEqual(result.returncode,0)
            self.assertIn('disorder exceeds',result.stderr)
            self.assertFalse((root/'output/summary.json').exists())


if __name__=='__main__': unittest.main()
