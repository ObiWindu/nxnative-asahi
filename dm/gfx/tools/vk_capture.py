#!/usr/bin/env python3
"""Validate Linux Vulkan shader captures and write a deterministic SPIR-V inventory.

Capture output is produced by src/graphics/vkapi/vk_api.c. The SPIR-V itself
contains descriptor and specialization declarations; pipeline-use sidecars add
the stage, entry point, and concrete specialization values.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


REPO = Path(__file__).resolve().parents[3]
SPIRV_CROSS = REPO / 'dm/gfx/tools/spirv_cross_bin/spirv-cross'
STAGES = {'vert', 'tesc', 'tese', 'geom', 'frag', 'comp', 'other'}
MAGIC = b'\x03\x02\x23\x07'


class CaptureError(ValueError):
    pass


def _read_json(path):
    try:
        return json.loads(path.read_text(encoding='utf-8'))
    except (OSError, UnicodeError, json.JSONDecodeError) as e:
        raise CaptureError(f'{path}: cannot read capture metadata: {e}') from e


def _module(root, path, spirv_cross):
    key = path.stem
    if len(key) != 16 or any(c not in '0123456789abcdef' for c in key):
        raise CaptureError(f'{path}: filename must be the first 16 lowercase SHA-256 digits')
    try:
        data = path.read_bytes()
    except OSError as e:
        raise CaptureError(f'{path}: cannot read SPIR-V: {e}') from e
    if len(data) < 20 or len(data) % 4 or data[:4] != MAGIC:
        raise CaptureError(f'{path}: invalid SPIR-V header or byte length')
    digest = hashlib.sha256(data).hexdigest()
    if digest[:16] != key:
        raise CaptureError(f'{path}: content digest does not match its filename')
    side_path = root / f'{key}.json'
    side = _read_json(side_path)
    if side.get('schema') != 1 or side.get('key') != key or side.get('sha256') != digest or side.get('code_size') != len(data):
        raise CaptureError(f'{side_path}: identity, digest, schema, or size does not match the SPIR-V')
    uses = []
    for use_path in sorted(root.glob(f'{key}.use-*.json')):
        raw = use_path.read_bytes()
        use_key = use_path.name[len(key) + 5:-5]
        if len(use_key) != 16 or hashlib.sha256(raw).hexdigest()[:16] != use_key:
            raise CaptureError(f'{use_path}: content digest does not match its filename')
        use = _read_json(use_path)
        if use.get('schema') != 1 or use.get('key') != key or use.get('stage') not in STAGES or not isinstance(use.get('entry_point'), str) or not use['entry_point']:
            raise CaptureError(f'{use_path}: invalid stage metadata')
        try:
            spec = bytes.fromhex(use.get('specialization_data_hex', ''))
        except ValueError as e:
            raise CaptureError(f'{use_path}: specialization data is not hex') from e
        entries = use.get('specialization_entries')
        if not isinstance(entries, list):
            raise CaptureError(f'{use_path}: specialization entries must be a list')
        seen = set()
        for entry in entries:
            if not isinstance(entry, dict):
                raise CaptureError(f'{use_path}: malformed specialization entry')
            cid, off, size = entry.get('constant_id'), entry.get('offset'), entry.get('size')
            if not all(isinstance(v, int) and v >= 0 for v in (cid, off, size)) or off + size > len(spec) or cid in seen:
                raise CaptureError(f'{use_path}: specialization entry is out of bounds or duplicated')
            seen.add(cid)
        uses.append(use)
    try:
        result = subprocess.run([str(spirv_cross), str(path), '--reflect'], capture_output=True, text=True, check=True, timeout=60)
        reflection = json.loads(result.stdout)
    except (OSError, subprocess.SubprocessError, json.JSONDecodeError) as e:
        detail = getattr(e, 'stderr', '')
        raise CaptureError(f'{path}: SPIRV-Cross reflection failed: {detail or e}') from e
    entries = reflection.get('entryPoints', [])
    if not isinstance(entries, list) or not entries:
        raise CaptureError(f'{path}: reflection has no entry point')
    declared = {(e.get('mode'), e.get('name')) for e in entries if isinstance(e, dict)}
    stage_mode = {'vert': 'vert', 'tesc': 'tesc', 'tese': 'tese', 'geom': 'geom', 'frag': 'frag', 'comp': 'comp'}
    for use in uses:
        mode = stage_mode.get(use['stage'])
        if mode and (mode, use['entry_point']) not in declared:
            raise CaptureError(f'{path}: captured use {use["stage"]}:{use["entry_point"]} is absent from SPIR-V reflection')
    return {
        'key': key,
        'sha256': digest,
        'code_size': len(data),
        'title_id': side.get('title_id') or None,
        'main_build_id': side.get('build_id') or None,
        'uses': uses,
        'reflection': reflection,
    }


def _layout(root, key):
    if not isinstance(key, str) or len(key) != 16 or any(c not in '0123456789abcdef' for c in key):
        raise CaptureError(f'{root}: invalid pipeline layout key {key!r}')
    path = root / f'layout-{key}.json'
    try:
        raw = path.read_bytes()
    except OSError as e:
        raise CaptureError(f'{path}: captured pipeline layout is missing: {e}') from e
    if hashlib.sha256(raw).hexdigest()[:16] != key:
        raise CaptureError(f'{path}: content digest does not match its filename')
    try:
        data = json.loads(raw)
    except (UnicodeError, json.JSONDecodeError) as e:
        raise CaptureError(f'{path}: invalid layout JSON: {e}') from e
    if data.get('schema') != 1 or data.get('kind') != 'pipeline_layout' or not isinstance(data.get('set_layouts'), list) or not isinstance(data.get('push_constant_ranges'), list):
        raise CaptureError(f'{path}: invalid pipeline layout metadata')
    for i, set_layout in enumerate(data['set_layouts']):
        if not isinstance(set_layout, dict):
            raise CaptureError(f'{path}: malformed descriptor set layout {i}')
        if set_layout.get('key') is not None and (not isinstance(set_layout.get('layout'), dict) or set_layout['layout'].get('kind') != 'descriptor_set_layout'):
            raise CaptureError(f'{path}: malformed descriptor set layout {i}')
    for i, item in enumerate(data['push_constant_ranges']):
        if not isinstance(item, dict) or any(not isinstance(item.get(k), int) or item[k] < 0 for k in ('stage_flags', 'offset', 'size')):
            raise CaptureError(f'{path}: malformed push constant range {i}')
    return data


def expected_keys(path):
    keys = []
    try:
        lines = Path(path).read_text(encoding='utf-8').splitlines()
    except (OSError, UnicodeError) as e:
        raise CaptureError(f'{path}: cannot read expected shader keys: {e}') from e
    for line_no, line in enumerate(lines, 1):
        key = line.split('#', 1)[0].strip()
        if not key:
            continue
        if len(key) != 16 or any(c not in '0123456789abcdef' for c in key):
            raise CaptureError(f'{path}:{line_no}: expected shader key must be 16 lowercase hex digits')
        keys.append(key)
    if len(keys) != len(set(keys)):
        raise CaptureError(f'{path}: duplicate shader keys')
    return sorted(keys)


def inventory(root, spirv_cross=SPIRV_CROSS, expected_tsv=None, require_complete=False):
    root = Path(root).resolve()
    if not root.is_dir():
        raise CaptureError(f'{root}: capture directory does not exist')
    modules = [_module(root, p, spirv_cross) for p in sorted(root.glob('*.spv'))]
    if not modules:
        raise CaptureError(f'{root}: no captured SPIR-V modules')
    ids = {m['title_id'] for m in modules if m['title_id']}
    builds = {m['main_build_id'] for m in modules if m['main_build_id']}
    if len(ids) > 1 or len(builds) > 1:
        raise CaptureError(f'{root}: capture mixes title/build identities')
    layout_keys = sorted({use['pipeline_layout_key'] for module in modules for use in module['uses'] if use.get('pipeline_layout_key') is not None})
    layouts = {key: _layout(root, key) for key in layout_keys}
    coverage = None
    if expected_tsv:
        expected = set(expected_keys(expected_tsv))
        captured = {m['key'] for m in modules}
        coverage = {
            'expected_count': len(expected),
            'captured_expected_count': len(expected & captured),
            'missing': sorted(expected - captured),
            'unexpected': sorted(captured - expected),
        }
        if require_complete and coverage['missing']:
            raise CaptureError(f'{root}: capture is missing {len(coverage["missing"])} of {len(expected)} expected shader modules; first missing key {coverage["missing"][0]}')
    elif require_complete:
        raise CaptureError('--require-complete needs --expected-tsv')
    return {
        'schema': 1,
        'capture_format': 'nxnative-vulkan-spirv-v1',
        'title_id': next(iter(ids), None),
        'main_build_id': next(iter(builds), None),
        'module_count': len(modules),
        'pipeline_use_count': sum(len(m['uses']) for m in modules),
        'pipeline_layout_count': len(layouts),
        'pipeline_layouts': layouts,
        'coverage': coverage,
        'modules': modules,
    }


def write_inventory(root, output, copy_to=None, spirv_cross=SPIRV_CROSS, expected_tsv=None, require_complete=False):
    root, output = Path(root).resolve(), Path(output).resolve()
    report = inventory(root, spirv_cross, expected_tsv, require_complete)
    encoded = (json.dumps(report, sort_keys=True, indent=2, ensure_ascii=True) + '\n').encode()
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=output.parent, prefix=output.name + '.', delete=False) as f:
        temp = Path(f.name)
        f.write(encoded); f.flush()
    temp.replace(output)
    if copy_to is not None:
        dst = Path(copy_to).resolve()
        dst.parent.mkdir(parents=True, exist_ok=True)
        stage = Path(tempfile.mkdtemp(prefix=dst.name + '.tmp-', dir=dst.parent))
        backup = None
        names = [p.name for p in root.glob('*.spv')]
        keys = {Path(n).stem for n in names}
        names += [f'{k}.json' for k in keys]
        names += [p.name for k in keys for p in root.glob(f'{k}.use-*.json')]
        names += [f'layout-{k}.json' for k in report['pipeline_layouts']]
        try:
            for name in sorted(set(names)):
                shutil.copyfile(root / name, stage / name)
            shutil.copyfile(output, stage / 'manifest.json')
            if dst.exists() or dst.is_symlink():
                backup = Path(tempfile.mkdtemp(prefix=dst.name + '.old-', dir=dst.parent))
                backup.rmdir()
                dst.replace(backup)
            try:
                stage.replace(dst)
            except OSError:
                if backup and (backup.exists() or backup.is_symlink()): backup.replace(dst)
                raise
            if backup:
                if backup.is_dir() and not backup.is_symlink(): shutil.rmtree(backup)
                else: backup.unlink()
        finally:
            if stage.exists(): shutil.rmtree(stage)
    return report


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('capture_dir', type=Path)
    ap.add_argument('--output', type=Path, required=True, help='write the validated deterministic JSON inventory here')
    ap.add_argument('--copy-to', type=Path, help='copy validated SPIR-V and sidecars with the inventory into this directory')
    ap.add_argument('--spirv-cross', type=Path, default=SPIRV_CROSS)
    ap.add_argument('--expected-tsv', type=Path, help='profile key list for captured-shader coverage')
    ap.add_argument('--require-complete', action='store_true', help='fail if any listed key is missing')
    a = ap.parse_args(argv)
    try:
        r = write_inventory(a.capture_dir, a.output, a.copy_to, a.spirv_cross, a.expected_tsv, a.require_complete)
    except CaptureError as e:
        sys.exit(f'[RED] {e}')
    print(f'{r["module_count"]} modules, {r["pipeline_use_count"]} pipeline-stage uses → {a.output}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
