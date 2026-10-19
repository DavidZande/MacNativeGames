"""Exercise the production batched frame logger without a game or a GPU."""
import csv
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('xcrun'), 'Darwin performance logger is Mac-only')
class FrameSamples(unittest.TestCase):
    def test_native_logger_preserves_ids_durations_and_high_frame_counts(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            source = work/'probe.c'
            source.write_text('''#include <stdint.h>
#include <unistd.h>
#include "totk_perf.h"
uint64_t g_draw_skip[16], g_async_pso_skipped, g_async_pso_kicked;
uint64_t g_inplace_pso_n, g_inplace_pso_ns, g_inplace_pso_max_ns, g_inplace_merged_n;
int mfxup_enabled(void) { return 0; }
int mfxup_output_enabled(void) { return 0; }
int mfx_requested(void) { return 0; }
int mtl_world_seen(void) { return 1; }
uint64_t totk_rt_block_ns(void) { return 0; }
uint64_t totk_rt_pending(void) { return 0; }
void mtl_stats(uint64_t *passes, uint64_t *draws) { *passes=0; *draws=0; }
uint64_t runtime_audio_underruns(void) { return 0; }
void runtime_gate_counts(uint64_t *timer, uint64_t *svc) { *timer=0; *svc=0; }
void runtime_handoff_stats(uint64_t out[16]) { for (unsigned i=0; i<16; i++) out[i]=0; }
int runtime_output_mode_get(void) { return 0; }
int main(void) {
 for (unsigned i=0; i<600; i++) totk_perf_frame(1000+i, i==42?100.0:4.0, 3.0, 0, 0, 0, i<2);
 usleep(1500000);
 return 0;
}
''')
            native = ROOT/'games/totk/third_party/d2r_gfx/gfx/nvn'
            binary = work/'probe'
            compiled = subprocess.run(['xcrun', 'clang', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-I'+str(native),
                                       str(source), str(native/'totk_perf.c'), '-o', str(binary)], capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            output = work/'frames.csv'
            env = {**os.environ, 'TOTK_PERF_CSV': str(work/'perf.csv'), 'TOTK_FRAME_SAMPLES': str(output), 'TOTK_PERF_LOG': '1'}
            env.pop('TOTK_HEADLESS', None)
            subprocess.run([str(binary)], env=env, check=True, capture_output=True)
            with output.open() as f: rows = list(csv.DictReader(f))
            self.assertEqual(len(rows), 600)
            self.assertEqual([int(r['frame']) for r in rows], list(range(1000, 1600)))
            self.assertEqual(float(rows[42]['frame_ms']), 100)
            self.assertEqual(float(rows[0]['frame_ms']), 4)
            self.assertEqual(sum(int(r['loading']) for r in rows), 2)
            times = [int(r['retired_ns']) for r in rows]
            self.assertGreater(times[0], 0)
            self.assertEqual(times, sorted(times))


if __name__ == '__main__':
    unittest.main()
