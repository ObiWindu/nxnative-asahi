"""The repository's two C# tools are committed as native executables (dm/tools/native_tools.sh, NativeAOT), so preparing a game needs no .NET:
the shader translator dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate and the extractor dm/tools/nx_extract_bin/nx_extract. On this Asahi fork they are AArch64 Linux ELF
executables, each built from the sources beside it, which it embeds and names with --source-sha: the translator also matches dmsh_chain.lock."""
from pathlib import Path
import hashlib
import importlib.util
import struct
import subprocess
import sys
import unittest

REPO = Path(__file__).resolve().parents[3]
XLATE = REPO / 'dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate'
NX = REPO / 'dm/tools/nx_extract_bin/nx_extract'
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
import dmsh_chain   # noqa: E402


def elf_arch(data):
    assert data[:4] == b'\x7fELF', 'not an ELF executable'
    assert data[4] == 2, 'not a 64-bit ELF executable'
    assert data[5] == 1, 'not little-endian'
    e_machine = struct.unpack_from('<H', data, 18)[0]
    return {183: 'aarch64', 62: 'x86_64'}.get(e_machine, f'?{e_machine}')


def nsp_script():
    spec = importlib.util.spec_from_file_location('build_ipa_from_nsp_native', REPO / 'scripts/build_ipa_from_nsp.py')
    assert spec and spec.loader
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
    return m


class NativeTools(unittest.TestCase):
    def test_native_executable(self):
        for exe in (XLATE, NX):
            with self.subTest(exe=exe.name):
                self.assertEqual(elf_arch(exe.read_bytes()), 'aarch64')
                self.assertTrue(exe.stat().st_mode & 0o111, f'{exe} is not executable')

    def test_only_the_executable_is_committed(self):   # no dlls, runtimeconfig or satellite folders of a framework-dependent build left beside it
        for exe in (XLATE, NX):
            with self.subTest(exe=exe.name): self.assertEqual(sorted(p.name for p in exe.parent.iterdir() if not p.name.startswith('.')), [exe.name])

    def test_projects_publish_native(self):
        for proj in ('dm/gfx/tools/dmsh_xlate/dmsh_xlate.csproj', 'dm/tools/nx_extract/nx_extract.csproj'):
            with self.subTest(proj=proj): self.assertIn('<PublishAot>true</PublishAot>', (REPO / proj).read_text())

    def test_translator_matches_the_lock(self):   # dmsh_build.py refuses another translator (tool_lock.py); this says so before any game
        with open(dmsh_chain.LOCK) as f: lock = dict(l.rstrip('\n').split('\t', 1) for l in f if '\t' in l and not l.startswith('#'))
        self.assertEqual(hashlib.sha256(XLATE.read_bytes()).hexdigest()[:16], lock.get('dmsh_xlate'), 'rebuilt translator: tool_lock.py --update-lock and dmsh_chain.py --header')
        self.assertEqual(lock.get('xlate_source'), dmsh_chain.xlate_source_sha(), 'dmsh_xlate sources changed: rebuild it with dm/tools/native_tools.sh dmsh_xlate')

    def test_built_from_the_sources_beside_them(self):
        want = {XLATE: dmsh_chain.xlate_source_sha(), NX: nsp_script().nx_extract_source_sha(REPO)}
        for exe, sha in want.items():
            with self.subTest(exe=exe.name):
                got = subprocess.run([str(exe), '--source-sha'], capture_output=True, text=True, timeout=60).stdout.strip()
                self.assertEqual(got, sha, f'{exe.name} was not built from its in-repo sources: rebuild it with dm/tools/native_tools.sh {exe.name}')


if __name__ == '__main__': unittest.main()
