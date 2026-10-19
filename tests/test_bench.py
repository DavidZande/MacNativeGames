"""Benchmark controls must not hide FPS ceilings or discard slow frames."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1]/'games/totk/scripts/bench.py'
spec = importlib.util.spec_from_file_location('bench', SCRIPT)
bench = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bench)


class UncappedBench(unittest.TestCase):
    def test_resolution_and_diagnostics_keep_all_pacing_disabled(self):
        for name in ('900p-unl', '1080p-unl', '1440p-unl', '720p-mfx-unl', '900p-unl-sync', 'handheld-unl'):
            with self.subTest(name=name):
                env = bench.config_env(name)
                self.assertEqual(env['DM_PRESENT_HZ'], '0')
                self.assertEqual(env['DM_PRESENT_LINK'], '0')
                self.assertEqual(env['TOTK_PRESENT_SKIP'], '1')
                self.assertEqual(env['TOTK_UNCAPPED'], '0')   # do not use the separate 120 Hz clock
                self.assertEqual(env['TOTK_MFX'], '0')        # do not count interpolated frames

    def test_old_caps_and_conflicting_overrides_are_rejected(self):
        for name in ('900p-60', '900p-30', '900p-unl-fine'):
            with self.assertRaises(SystemExit): bench.config_env(name)
        for override in ('DM_PRESENT_HZ=60', 'DM_PRESENT_LINK=1', 'TOTK_PRESENT_SKIP=0', 'TOTK_UNCAPPED=1', 'TOTK_MFX=1'):
            with self.assertRaises(SystemExit): bench.extra_env([override])
        self.assertEqual(bench.extra_env(['TOTK_TRACE_VAMM=1']), {'TOTK_TRACE_VAMM': '1'})

    def test_full_distribution_preserves_hitch_and_fps_above_display_rate(self):
        frames = [dict(frame_ms='5', gpu_ms='4') for _ in range(99)] + [dict(frame_ms='100', gpu_ms='4')]
        result = bench.summarize([{'fps': '60', 'frame_p99_ms': '5'}], frames)
        self.assertEqual(result['sample_count'], 100)
        self.assertEqual(result['render_fps'], round(1000 * 100 / 595, 2))
        self.assertGreater(result['render_fps'], 120)
        self.assertEqual(result['p99_ms'], 100)
        self.assertEqual(result['long50'], 1)
        self.assertEqual(result['max_ms'], 100)

    def test_sample_boundaries_overflow_and_incomplete_write(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)/'frames.csv'
            path.write_text('frame,retired_ns,frame_ms,gpu_ms,loading\n1,99,5,4,0\n2,100,5,4,0\n3,200,5,4,0\n4,')
            self.assertEqual([r['frame'] for r in bench.sample_rows(path, 100, 200)], ['2'])
            with path.open('a') as f: f.write('\n# overflow=1\n')
            with self.assertRaisesRegex(RuntimeError, 'overflow'): bench.sample_rows(path, 0, 300)
        with self.assertRaisesRegex(RuntimeError, 'no per-frame samples'): bench.summarize([{'fps': '60'}], [])


if __name__ == '__main__':
    unittest.main()
