import hashlib
import importlib.util
import json
from pathlib import Path
import struct
from types import SimpleNamespace
from unittest.mock import patch

import pytest


ROOT = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location('vk_capture', ROOT / 'dm/gfx/tools/vk_capture.py')
vk_capture = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(vk_capture)


def sample_capture(root, *, bad_use=False):
    root.mkdir()
    code = struct.pack('<5I', 0x07230203, 0x00010000, 0, 2, 0)
    digest = hashlib.sha256(code).hexdigest()
    key = digest[:16]
    (root / f'{key}.spv').write_bytes(code)
    (root / f'{key}.json').write_text(json.dumps({
        'schema': 1, 'key': key, 'sha256': digest, 'code_size': len(code),
        'title_id': '0100000000000000', 'build_id': '00112233445566778899aabbccddeeff',
    }))
    layout = {'schema': 1, 'kind': 'pipeline_layout', 'flags': 0, 'set_layouts': [],
              'push_constant_ranges': [{'stage_flags': 1, 'offset': 0, 'size': 16}]}
    layout_raw = (json.dumps(layout, separators=(',', ':')) + '\n').encode()
    layout_key = hashlib.sha256(layout_raw).hexdigest()[:16]
    (root / f'layout-{layout_key}.json').write_bytes(layout_raw)
    use = {
        'schema': 1, 'key': key, 'stage': 'vert', 'stage_flags': 1,
        'entry_point': 'main', 'pipeline_layout_key': layout_key,
        'specialization_entries': [], 'specialization_data_hex': '',
    }
    raw = (json.dumps(use, separators=(',', ':')) + '\n').encode()
    use_key = hashlib.sha256(raw).hexdigest()[:16]
    (root / f'{key}.use-{use_key}.json').write_bytes(raw)
    return key, code, layout_key


def reflect(_cmd, **_kw):
    return SimpleNamespace(stdout=json.dumps({'entryPoints': [{'mode': 'vert', 'name': 'main'}], 'types': {},
                                                'ubos': [{'type': 1, 'set': 0, 'binding': 2}],
                                                'specialization_constants': [{'id': 4, 'type': 'uint'}]}), stderr='')


def test_inventory_reflects_and_copies_deterministically(tmp_path):
    source = tmp_path / 'capture'; key, code, layout_key = sample_capture(source)
    out1, out2 = tmp_path / 'first.json', tmp_path / 'second.json'
    package = tmp_path / 'package'
    package.mkdir(); (package / 'stale.spv').write_bytes(b'stale')
    with patch.object(vk_capture.subprocess, 'run', side_effect=reflect) as run:
        report = vk_capture.write_inventory(source, out1, package)
        vk_capture.write_inventory(source, out2)
    assert out1.read_bytes() == out2.read_bytes()
    assert report['module_count'] == 1 and report['pipeline_use_count'] == 1
    assert report['pipeline_layout_count'] == 1
    assert report['pipeline_layouts'][layout_key]['push_constant_ranges'][0]['size'] == 16
    module = report['modules'][0]
    assert module['key'] == key and module['code_size'] == len(code)
    assert module['title_id'] == '0100000000000000'
    assert module['uses'][0]['stage'] == 'vert'
    assert module['reflection']['ubos'][0]['binding'] == 2
    assert (package / f'{key}.spv').read_bytes() == code
    assert (package / f'layout-{layout_key}.json').is_file()
    assert not (package / 'stale.spv').exists()
    assert json.loads((package / 'manifest.json').read_text()) == report
    assert run.call_count == 2


def test_inventory_rejects_filename_digest_mismatch(tmp_path):
    source = tmp_path / 'capture'; sample_capture(source)
    path = next(source.glob('*.spv'))
    path.write_bytes(path.read_bytes() + b'\0\0\0\0')
    with pytest.raises(vk_capture.CaptureError, match='digest does not match'):
        vk_capture.inventory(source)


def test_inventory_rejects_specialization_data_outside_capture(tmp_path):
    source = tmp_path / 'capture'; key, _, _ = sample_capture(source)
    use_path = next(source.glob('*.use-*.json'))
    use = json.loads(use_path.read_text())
    use['specialization_entries'] = [{'constant_id': 1, 'offset': 4, 'size': 4}]
    use['specialization_data_hex'] = '0000'
    raw = (json.dumps(use, separators=(',', ':')) + '\n').encode()
    use_path.unlink()
    use_key = hashlib.sha256(raw).hexdigest()[:16]
    (source / f'{key}.use-{use_key}.json').write_bytes(raw)
    with pytest.raises(vk_capture.CaptureError, match='out of bounds'):
        vk_capture.inventory(source)


def test_profile_coverage_reports_missing_keys_and_can_require_full_capture(tmp_path):
    source = tmp_path / 'capture'; key, _, _ = sample_capture(source)
    expected = tmp_path / 'vk_capture.tsv'
    expected.write_text('# expected module digests\n' + key + '\n' + '0' * 16 + '\n')
    with patch.object(vk_capture.subprocess, 'run', side_effect=reflect):
        report = vk_capture.inventory(source, expected_tsv=expected)
        assert report['coverage']['captured_expected_count'] == 1
        assert report['coverage']['missing'] == ['0' * 16]
        with pytest.raises(vk_capture.CaptureError, match='missing 1 of 2'):
            vk_capture.inventory(source, expected_tsv=expected, require_complete=True)
