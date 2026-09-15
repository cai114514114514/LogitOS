"""Watch the apparatus reject old pixels, stale frames and missing evidence."""
import importlib.util
import pathlib
import unittest

path = pathlib.Path(__file__).resolve().parents[2] / 'tools/perf/browser_load.py'
spec = importlib.util.spec_from_file_location('browser_load', path)
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)

def frame(x=60):
    return ('[dl] ---8<--- begin painted text\r\n'
            f'[dl] {x},148 MEDIA700\r\n[dl] 100,202 MEDIANESTED\r\n'
            '[dl] ---8<--- end painted text\r\n')

class PaintProof(unittest.TestCase):
    def test_correct(self):
        self.assertEqual(runner.media_region_paint_proof(frame())['MEDIA700'], [60, 148])
    def test_old_pixels_rejected(self):
        with self.assertRaisesRegex(AssertionError, 'wrong media-gated'):
            runner.media_region_paint_proof(frame(920))
    def test_earlier_correct_frame_cannot_hide_final_bad_frame(self):
        with self.assertRaisesRegex(AssertionError, 'wrong media-gated'):
            runner.media_region_paint_proof(frame() + frame(920))
    def test_missing_paint(self):
        with self.assertRaisesRegex(AssertionError, 'no complete'):
            runner.media_region_paint_proof('CSS-MEDIA-REGIONS PASS\n')
    def test_incomplete_final_frame_does_not_use_earlier_frame(self):
        with self.assertRaisesRegex(AssertionError, 'incomplete final'):
            runner.media_region_paint_proof(frame() + '[dl] ---8<--- begin painted text\n')
    def test_incomplete_or_duplicate_label(self):
        for data in (frame().replace('MEDIANESTED', 'ABSENT'),
                     frame().replace('[dl] 100,202', '[dl] 60,148 MEDIA700\r\n[dl] 100,202')):
            with self.assertRaisesRegex(AssertionError, 'missing or ambiguous'):
                runner.media_region_paint_proof(data)

if __name__ == '__main__':
    unittest.main()
