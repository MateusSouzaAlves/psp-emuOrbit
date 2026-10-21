"""One reviewed GLES fix in a derived compilation unit; pristine pin untouched.

ShaderManager must not request GL_CLIP_DISTANCE0 when VertexShaderGenerator
does not emit a clip-distance output. No renderer feature is fabricated and no
GL error, compatibility guard or saved state is suppressed.
"""
from pathlib import Path
import hashlib

BEFORE = 'if (!VSID.Bit(VS_BIT_IS_THROUGH) && gstate_c.Use(GPU_USE_DEPTH_CLAMP)) {'
AFTER = 'if (!VSID.Bit(VS_BIT_IS_THROUGH) && gstate_c.Use(GPU_USE_DEPTH_CLAMP) && gstate_c.Use(GPU_USE_CLIP_DISTANCE)) {'
GENERATOR = 'bool clipClampedDepth = !isModeThrough && gstate_c.Use(GPU_USE_DEPTH_CLAMP) && gstate_c.Use(GPU_USE_CLIP_DISTANCE);'
UPSTREAM_SHA256 = '1c99dc56a43a16b8ddbc6192e0f130d7bdfbb8c6407d8366d92297ac351c7281'
DERIVED_SHA256 = 'ac8d2516f3132444e8f2533359eea2da42bd4401c07183d123642fa5c7aed14b'


def contract() -> dict:
    return {'id': 'gles-depth-clamp-clip-distance-1',
            'upstream_file': 'GPU/GLES/ShaderManagerGLES.cpp',
            'upstream_sha256': UPSTREAM_SHA256, 'derived_sha256': DERIVED_SHA256,
            'change': 'Require actual clip-distance support for the depth-clamp program flag, matching the unchanged shader generator.'}


def patch_bytes(data: bytes, generator: str) -> bytes:
    needle, replacement = BEFORE.encode(), AFTER.encode()
    if data.count(needle) != 1 or replacement in data or generator.count(GENERATOR) != 1:
        raise ValueError('Pinned shader contract differs; never guess a patch')
    return data.replace(needle, replacement)


def prepare(source: Path, output: Path) -> dict:
    original = source / 'GPU/GLES/ShaderManagerGLES.cpp'
    generator = source / 'GPU/Common/VertexShaderGenerator.cpp'
    if source.resolve() in output.resolve().parents or output.resolve() == original.resolve():
        raise ValueError('Derived shader must remain outside pristine source')
    data = original.read_bytes()
    if hashlib.sha256(data).hexdigest() != UPSTREAM_SHA256:
        raise ValueError('Pinned shader-manager source differs; never guess a patch')
    derived = patch_bytes(data, generator.read_text('utf-8'))
    if hashlib.sha256(derived).hexdigest() != DERIVED_SHA256:
        raise ValueError('Derived shader differs from reviewed one-line fix')
    output.parent.mkdir(parents=True, exist_ok=True)
    # Generated build input, not upstream or a runtime state. Identical retries
    # don't advance its mtime and unnecessarily rebuild the whole core.
    if not output.exists() or output.read_bytes() != derived:
        output.write_bytes(derived)
    return contract()
