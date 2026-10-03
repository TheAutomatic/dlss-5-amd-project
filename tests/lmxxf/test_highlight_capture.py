import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('highlight', Path(__file__).resolve().parents[2] / 'tools/diag/analyze-highlight.py')
highlight = importlib.util.module_from_spec(spec)
spec.loader.exec_module(highlight)


class CaptureTests(unittest.TestCase):
    def test_v2_large_roi_and_guides(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'test.nrhl'
            data = bytearray(b'NRHLV2\0\0')
            for stage, fmt, size, pixel in ((0, 10, 128, struct.pack('<4e', 2, 2, 2, 1)),
                                           (5, 2, 128, struct.pack('<4f', .25, -.5, .75, 1))):
                data.extend(highlight.HEADER.pack(7, stage, 0, 1920, 1080, 100, 200, size, size, fmt, 2, 0,
                                                  1, 1, 1, 1, 1, 1000))
                data.extend(pixel * size * size)
            path.write_bytes(data)
            rows = list(highlight.summaries(highlight.read_records(path)))
            self.assertAlmostEqual(rows[0]['mean'], 2)
            self.assertEqual(rows[1]['stage'], 'motion_depth')
            self.assertAlmostEqual(rows[1]['mean'], .75)
            path.write_bytes(data[:-1])
            with self.assertRaisesRegex(ValueError, 'Truncated pixel'):
                list(highlight.read_records(path))

    def test_real_half_float_and_temporal_gap(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'test.nrhl'
            data = bytearray(b'NRHLV1\0\0')
            for frame, value in ((1, 1.), (2, 2.), (4, 4.)):
                data.extend(highlight.HEADER.pack(frame, 0, 0, 1, 1, 0, 0, 1, 1, 10, 3, 0,
                                                  1., 1., 8., 1., 1., frame))
                data.extend(struct.pack('<4e', value, value, value, 1.))
            path.write_bytes(data)
            rows = list(highlight.summaries(highlight.read_records(path)))
            self.assertAlmostEqual(rows[0]['mean'], 1.)
            self.assertAlmostEqual(rows[1]['mean_abs_delta'], 1.)
            self.assertEqual(rows[2]['mean_abs_delta'], '')
            path.write_bytes(data[:-1])
            with self.assertRaisesRegex(ValueError, 'Truncated pixel'):
                list(highlight.read_records(path))

    def test_nonfinite_not_silently_counted_as_black(self):
        r = dict(zip(highlight.FIELDS, (1, 0, 0, 2, 1, 0, 0, 2, 1, 10, 3, 0, 1., 1., 1., 1., 1., 0)))
        row = next(highlight.summaries([(r, [float('nan'), 3.])]))
        self.assertEqual(row['nonfinite'], 1)
        self.assertEqual(row['mean'], 3.)


if __name__ == '__main__':
    unittest.main()
