"""Linux importer graphics-workflow selection from detected game inputs."""
from pathlib import Path
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


def test_nvn_runtime_capture_is_still_rejected():
    with pytest.raises(SystemExit, match='GLSLC capture'):
        asahi.shader_workflow(source(glslc_capture=(True, 'runtime GLSL')))


def test_opengl_remains_explicitly_unsupported():
    with pytest.raises(SystemExit, match='supports NVN shaders and packaged direct-Vulkan SPIR-V'):
        asahi.shader_workflow(source('gl'))
