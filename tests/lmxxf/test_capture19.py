import importlib.util
import tempfile
import unittest
import os
import subprocess
import zipfile
from pathlib import Path

ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('fullframes',ROOT/'tools/diag/read-fullframes.py')
mod=importlib.util.module_from_spec(spec);spec.loader.exec_module(mod)

class Capture19Tests(unittest.TestCase):
    def test_pair_and_corruption(self):
        def record(stage):return mod.HEADER.pack(17,stage,0,4,3,0,0,4,3,10,2,0,1,1,1,1,1,1234)+bytes(4*3*8)
        data=b'NRFFV1\0\0'+record(1)+record(2)
        with tempfile.TemporaryDirectory() as folder:
            p=Path(folder)/'sample.full';p.write_bytes(data)
            rows=mod.index(p);self.assertEqual([r['stage'] for r in rows],[1,2]);self.assertEqual(rows[0]['bytes'],96)
            for bad in (data[:-1],data[:-96],b'BADMAGIC'+data[8:],b'NRFFV1\0\0'+record(1),b'NRFFV1\0\0'+record(2)+record(1)):
                p.write_bytes(bad)
                with self.assertRaises(ValueError):mod.index(p)

    @unittest.skipUnless(os.name=='nt','Windows collector')
    def test_collector_complete_missing_rows_and_no_capture(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);game=root/'game';captures=root/'captures';out=root/'out'
            for p in (game,captures,out):p.mkdir()
            (game/'OptiScaler.log').write_text('fixture log')
            base=captures/'capture-1-1-1-mode0.nrhl'
            def record(stage,frame=17):return mod.HEADER.pack(frame,stage,0,4,3,0,0,4,3,11 if stage==0 else 10,2,0,1,1,1,1,1,1234)+bytes(96)
            base.write_bytes(b'NRHLV2\0\0'+record(0)+record(1)+record(2)+record(3)+record(0,18)+record(1,18)+record(2,18)+record(3,18))
            Path(str(base)+'.full').write_bytes(b'NRFFV1\0\0'+record(1)+record(2))
            Path(str(base)+'.csv').write_text('frame,tick,mask\n17,1000,47\n18,20000,47\n')
            reuse=Path(str(base)+'.reuse.csv');reuse.write_text('frame,mode,reuse\n17,0,0\n18,0,0\n')
            report=Path(str(base)+'.info.txt');report.write_text('build=NR-test19\npixel_frames=2\ndiagnostics_complete=1\ncomplete=1\nfull_errors=0\nreuse_errors=0\nfull_frames=1\nfull_target=1\nreuse_requested=2\nreuse_completed=2\nread_frames=2\n')
            def collect():
                result=subprocess.run(['powershell.exe','-NoProfile','-ExecutionPolicy','Bypass','-File',str(ROOT/'tools/diag/collect-nr-test19.ps1'),'-GameDirectory',str(game),'-CaptureDirectory',str(captures),'-OutputDirectory',str(out)],capture_output=True)
                self.assertEqual(result.returncode,0,result.stderr.decode(errors="replace"))
                archive=max(out.glob('*.zip'),key=lambda p:p.stat().st_mtime_ns)
                with zipfile.ZipFile(archive) as z:
                    return z.read(next(n for n in z.namelist() if n.endswith('CAPTURE-CHECK.txt'))).decode('utf-8-sig')
            self.assertIn('\nCOMPLETE:',collect())
            Path(str(base)+'.csv').write_text('frame,tick,mask\n17,1000,47\n18,2000,47\n')
            self.assertIn('INCOMPLETE:',collect())
            reuse.write_text('frame,mode,reuse\n19,0,0\n')
            self.assertIn('INCOMPLETE:',collect())
            report.unlink()
            self.assertIn('NO SAVED CAPTURE:',collect())

if __name__=='__main__':unittest.main()
