"""Compute warm-up discovers unplayed variants without accepting stale reflection."""
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('xcrun'), 'Foundation probe is Mac-only')
class ComputeWarm(unittest.TestCase):
    def test_cached_compute_selection(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            cache = work/'cache'
            cache.mkdir()
            chain = 'totk-native-v4-unverified'
            fixtures = {
                'unplayed_b': dict(chain=chain, stage='cs'),
                'binding_variant_b': dict(chain=chain, stage='cs'),
                'vertex_b': dict(chain=chain, stage='vs'),
                'fragment_b': dict(chain=chain, stage='ps'),
                'stale_b': dict(chain='old-chain', stage='cs'),
                'unfinished_b': None,
                'malformed_b': [],
                'broken_b': 'invalid json',
            }
            for key, reflection in fixtures.items():
                (cache/(key+'.metal')).write_text('// fixture')
                if reflection is not None:
                    (cache/(key+'.json')).write_text(reflection if isinstance(reflection, str) else json.dumps(reflection))
            # A reflection that has no source is not a warmable cached shader.
            (cache/'orphan_b.json').write_text(json.dumps(dict(chain=chain, stage='cs')))
            source = work/'probe.m'
            source.write_text('''#import "totk_compute_warm.h"
int main(int argc, const char **argv) { @autoreleasepool {
 if (argc != 2) return 2;
 NSString *cache = @(argv[1]);
 NSArray *files = [[NSFileManager.defaultManager contentsOfDirectoryAtPath:cache error:nil]
                    sortedArrayUsingSelector:@selector(compare:)];
 printf("%s", totk_compute_warm_manifest(cache, files).UTF8String);
 return 0;
} }
''')
            binary = work/'probe'
            subprocess.run(['xcrun', 'clang', '-fobjc-arc', '-Wall', '-Wextra', '-Werror',
                            '-I'+str(ROOT/'games/totk/third_party/d2r_gfx/gfx/nvn'),
                            str(source), '-framework', 'Foundation', '-o', str(binary)],
                           check=True, capture_output=True)
            result = subprocess.run([str(binary), str(cache)], check=True, capture_output=True, text=True)
            self.assertEqual(result.stdout.splitlines(), ['C\tbinding_variant_b', 'C\tunplayed_b'])


if __name__ == '__main__':
    unittest.main()
