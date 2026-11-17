"""Linux importer graphics-workflow selection from detected game inputs."""
from pathlib import Path
import struct
import subprocess
import sys

import pytest


ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'scripts'))
import build_asahi_from_nsp as asahi


def source(gfx='nvn', **overrides):
    row = {
        'graphics': (gfx, gfx),
        'glslc_capture': (False, ''),
        'vk_capture': (False, ''),
        'gl_capture': (False, ''),
        'ue_decode': (False, ''),
        'oodle_packs': (False, ''),
        'ue_legacy': (False, ''),
    }
    row.update(overrides)
    return row


def test_nvn_uses_translated_shader_cache():
    assert asahi.shader_workflow(source()) == 'nvn'


def test_packaged_direct_vulkan_spirv_needs_no_nvn_cache():
    assert asahi.shader_workflow(source('vulkan')) == 'vulkan'


def test_vulkan_runtime_capture_uses_native_spirv_workflow():
    assert asahi.shader_workflow(source('vulkan', vk_capture=(True, 'runtime SPIR-V'))) == 'vulkan'


def test_nvn_runtime_glslc_uses_native_shader_workflow():
    assert asahi.shader_workflow(source(glslc_capture=(True, 'runtime GLSL'))) == 'nvn'


def test_glslc_capture_reader_ignores_partial_outputs(tmp_path):
    valid = bytearray(0x48)
    struct.pack_into('<I', valid, 0, 0x19866891)
    struct.pack_into('<I', valid, 0x44, len(valid))
    (tmp_path / 'valid.glslc').write_bytes(valid)
    (tmp_path / 'partial.glslc').write_bytes(valid[:0x30])

    assert asahi.nsp.glslc_done(tmp_path) == {'valid.glslc'}


def test_unreal_shader_stamp_hashes_pak_and_iostore_payloads(tmp_path):
    romfs = tmp_path / 'RomFS'; romfs.mkdir()
    pak = romfs / 'Game.pak'; pak.write_bytes(b'pak!')
    toc = romfs / 'Game.utoc'; toc.write_bytes(b'toc!')
    data = romfs / 'Game.ucas'; data.write_bytes(b'ucas')
    first = asahi.nsp.ue_stamp(tmp_path)
    data.write_bytes(b'data')  # same length, different source contents
    second = asahi.nsp.ue_stamp(tmp_path)
    assert first != second
    assert 'Game.ucas\t4\t' in second


def test_generated_run_script_exports_capture_environment(tmp_path):
    host = tmp_path / 'fake host.sh'
    host.write_text('#!/bin/sh\nprintf "%s\\n" "$DM_GLSLC_CAPTURE"\n')
    host.chmod(0o755)
    capture = tmp_path / 'capture output'
    script = tmp_path / 'run.sh'
    script.write_text(asahi.game_run_script(host, tmp_path / 'patched main', {'DM_GLSLC_CAPTURE': capture}))
    script.chmod(0o755)

    result = subprocess.run([script], check=True, capture_output=True, text=True)
    assert result.stdout.strip() == str(capture)


@pytest.mark.parametrize('field', ['ue_decode', 'oodle_packs', 'ue_legacy'])
def test_nvn_game_owned_decoder_formats_use_nvn_shader_workflow(field):
    assert asahi.shader_workflow(source(**{field: (True, 'game-owned decoder')})) == 'nvn'


def test_nvn_open_gl_capture_is_reported_as_unsupported():
    with pytest.raises(SystemExit, match='OpenGL runtime shader capture'):
        asahi.shader_workflow(source(gl_capture=(True, 'runtime GLSL')))


def test_opengl_remains_explicitly_unsupported():
    with pytest.raises(SystemExit, match='supports NVN shaders and packaged direct-Vulkan SPIR-V'):
        asahi.shader_workflow(source('gl'))
