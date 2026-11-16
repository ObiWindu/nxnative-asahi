"""scripts/build_asahi_from_nsp.py on a NUP container: the container is scanned and unpacked, the
rewrite and shader/host stages are replaced by stubs, and the launcher-ready game folder is
assembled without keys. The NUP is the synthetic fixture of test_nsp_nup.py (no game files)."""
import importlib.util
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/tools'))
sys.path.insert(0, str(REPO / 'dm/tools/tests'))
sys.path.insert(0, str(REPO / 'dm/games'))
import test_nsp_nup as fixture
import profile_guess

spec = importlib.util.spec_from_file_location('build_asahi_from_nsp', REPO / 'scripts/build_asahi_from_nsp.py')
assert spec is not None and spec.loader is not None
builder = importlib.util.module_from_spec(spec); spec.loader.exec_module(builder)
nsp = builder.nsp


@unittest.skipIf(fixture.zstd is None, 'no zstd module')
class NupBuild(unittest.TestCase):
    def test_nup_reaches_a_launcher_ready_folder(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td).resolve(); src = root / 'game'; src.mkdir()
            nup = src / 'game.nup'
            fixture.write_nup(nup, [(0, fixture.TID, 0, fixture.version(marker='old'), fixture.META),
                                    (1, fixture.TID, fixture.VER, fixture.version(main=b'new code', marker='new'), b'')])
            work = root / 'work'

            def rewrite(py, repo, exe, patched, env, logs, g, provisional):
                patched.mkdir(parents=True, exist_ok=True)
                (patched / 'main.patched.bin').write_bytes(b'patched')
                (patched / 'main.fixups.tsv').write_text('# synthetic\n')
                return g

            def sources(exefs, rom, modules):
                return {'graphics': ('nvn', 'synthetic NVN'), 'glslc_capture': (False, ''),
                        'vk_capture': (False, ''), 'gl_capture': (False, ''), 'ue_decode': (False, ''),
                        'oodle_packs': (False, ''), 'ue_legacy': (False, '')}

            with mock.patch.object(nsp, 'rewrite', side_effect=rewrite), \
                 mock.patch.object(nsp, 'chain_header'), \
                 mock.patch.object(nsp, 'build_mismatch', return_value=[]), \
                 mock.patch.object(nsp, 'extract_aoc', return_value=None), \
                 mock.patch.object(nsp, 'split_shaders', side_effect=AssertionError('shaders are skipped')), \
                 mock.patch.object(profile_guess, 'build_sources', side_effect=sources):
                rc = builder.main([str(nup), '--work-dir', str(work), '--skip-shaders', '--skip-host'])
            self.assertEqual(rc, 0)
            game = work / 'game'
            self.assertTrue((game / 'game.tsv').is_file())
            self.assertTrue((game / 'main.patched.bin').is_file())
            self.assertTrue((game / 'main.fixups.tsv').is_file())
            self.assertTrue((game / 'romfs').exists())
            self.assertTrue((game / 'save').is_dir())
            self.assertTrue((work / 'run.sh').is_file())
            self.assertIn('NX_NVN_SHADER_CACHE=', (work / 'run.sh').read_text())


if __name__ == '__main__': unittest.main()
