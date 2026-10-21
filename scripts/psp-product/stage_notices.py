"""Pinned original notices, separate from PSP runtime assets and user data.

This conservative source-notice inventory is not a linked-component SBOM or a
legal conclusion about the combined app. Nothing is fetched or published here.
Inline notices are retained as whole original files, not paraphrased/extracted.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
MAX_FILES, MAX_FILE, MAX_TOTAL = 512, 2 * 1024 * 1024, 8 * 1024 * 1024
# Only the initialized, pinned component roots used by this native recipe and
# its packaged debugger assets. Do not recurse into unrelated/uninitialized
# desktop/test submodules, or silently omit a required component's checkout.
SELECTION = (
    'LICENSE.TXT', 'assets/debugger', 'ext/SPIRV-Cross', 'ext/armips',
    'ext/cityhash', 'ext/cpu_features', 'ext/gason', 'ext/glslang',
    'ext/libadrenotools', 'ext/libchdr', 'ext/lua', 'ext/naett', 'ext/nanosvg',
    'ext/rapidjson/license.txt', 'ext/rcheevos', 'ext/snappy', 'ext/udis86',
    'ext/zlib', 'ext/zstd', 'ffmpeg/LICENSE.md', 'ffmpeg/COPYING.GPLv2',
    'ffmpeg/COPYING.GPLv3', 'ffmpeg/COPYING.LGPLv2.1', 'ffmpeg/COPYING.LGPLv3',
    'libretro/libretro-common',
)
SUPPLEMENTS = (
    'ext/at3_standalone/README.txt', 'ext/at3_standalone/atrac3.cpp',
    'ext/at3_standalone/atrac3plus.cpp', 'ext/basis_universal/basisu.h',
    'ext/jpge/jpgd.cpp', 'ext/jpge/jpge.cpp', 'ext/libkirk/AES.c',
    'ext/libkirk/SHA1.c', 'ext/libkirk/amctrl.c', 'ext/libkirk/kirk_engine.c',
    'ext/libpng17/png.h', 'ext/libzip/zip.h', 'ext/lua/lua.h',
    'ext/minimp3/minimp3.h', 'ext/pugixml/pugixml.hpp',
    'ext/stb/stb_rect_pack.h', 'ext/vma/vk_mem_alloc.h', 'ext/xxhash.h',
    'libretro/libretro-common/include/libretro.h',
)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def valid_name(name):
    return (len(name) <= 240 and name.startswith('upstream/')
            and re.fullmatch(r'[A-Za-z0-9_./ -]+', name) is not None
            and all(part and part not in ('.', '..') and part == part.strip(' ')
                    and not part.endswith('.') for part in name.split('/')))


def is_notice(relative):
    path = Path(relative)
    return ('LICENSES' in path.parts
            or re.fullmatch(r'(?i)(?:licen[cs]e|copying|copyright|notice|authors|credits)'
                            r'(?:\.(?:txt|md|rst|adoc|gplv[23]|lgplv[23](?:\.1)?))?', path.name) is not None
            or path.name.endswith('.LICENSE.txt'))


def tracked_notices(repository, prefix='', selection=()):
    # Unlike runtime asset staging, traverse only to select notice files: an
    # unrelated source symlink must never be followed/copied or mistaken for a
    # missing license. A selected notice that is a symlink is still rejected.
    result = []
    listing = subprocess.check_output(['git', '-C', str(repository), 'ls-files', '--stage', '-z', '--', *selection])
    for record in listing.decode('utf-8').split('\0'):
        if not record:continue
        metadata, name = record.split('\t', 1)
        mode, expected, stage_number = metadata.split(' ')
        relative = prefix + name
        if stage_number != '0':raise ValueError('Unmerged PSP notice source')
        if mode == '160000':
            child = repository / name
            if not (child / '.git').exists():raise ValueError('Initialize required PSP notice submodule: ' + relative)
            git = lambda *args: subprocess.check_output(['git', '-C', str(child), *args], text=True).strip()
            if git('rev-parse', 'HEAD') != expected or git('diff', 'HEAD', '--'):
                raise ValueError('PSP notice submodule pin/source mismatch')
            result.extend(tracked_notices(child, relative + '/'))
        elif is_notice(relative) or relative in SUPPLEMENTS:
            if mode not in ('100644', '100755'):raise ValueError('PSP notice cannot be a symlink')
            result.append((repository / name, relative))
    return result


def manifest_entries(data):
    if not data or len(data) > 128 * 1024:
        raise ValueError('PSP notices manifest size')
    entries = []
    seen = set()
    for line in data.decode('ascii').splitlines():
        digest, name = line.split('  ', 1)
        if not re.fullmatch('[0-9a-f]{64}', digest) or not valid_name(name) or name.lower() in seen:
            raise ValueError('Unsafe or duplicate PSP notices manifest entry')
        seen.add(name.lower())
        entries.append((digest, name))
    if not 0 < len(entries) <= MAX_FILES:
        raise ValueError('PSP notices count bound')
    return entries


def verify(read, expected, actual_names=None):
    manifest = read('manifest.sha256')
    if sha(manifest) != expected['manifest_sha256']:
        raise ValueError('PSP notices manifest mismatch')
    entries = manifest_entries(manifest)
    total = 0
    for digest, name in entries:
        data = read(name)
        total += len(data)
        if len(data) > MAX_FILE or total > MAX_TOTAL or sha(data) != digest:
            raise ValueError('PSP original notice missing/changed/outside bound')
    if len(entries) != expected['files'] or total != expected['bytes']:
        raise ValueError('PSP notices inventory mismatch')
    names = {name for _, name in entries} | {'manifest.sha256'}
    if actual_names is not None and set(actual_names) != names:
        raise ValueError('Unexpected PSP notice outside manifest')


def stage_files(source, output, selected, revision):
    # Validate the complete bounded inventory before creating anything.
    entries, names, total = [], set(), 0
    for original, relative in sorted(selected, key=lambda item: item[1]):
        name = 'upstream/' + relative
        if not valid_name(name) or name.lower() in names:
            raise ValueError('Unsafe or duplicate PSP original notice path')
        if original.is_symlink() or not original.is_file() or source not in original.resolve().parents:
            raise ValueError('PSP notices must be regular pinned upstream files')
        data = original.read_bytes()
        total += len(data)
        if len(data) > MAX_FILE or total > MAX_TOTAL:
            raise ValueError('PSP original notices size bound')
        names.add(name.lower())
        entries.append((original, name, sha(data)))
    manifest = ''.join(f'{digest}  {name}\n' for _, name, digest in entries).encode('ascii')
    manifest_entries(manifest)
    output.mkdir(parents=True, exist_ok=False)
    for original, name, digest in entries:
        target = output / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(original, target)
        if sha(target.read_bytes()) != digest or sha(original.read_bytes()) != digest:
            raise ValueError('Pinned original notice changed during staging')
    (output / 'manifest.sha256').write_bytes(manifest)
    receipt = {'core_pin': revision, 'files': len(entries), 'bytes': total,
               'manifest_sha256': sha(manifest),
               'empty_originals': [name for _, name, digest in entries if digest == sha(b'')],
               'scope': 'CONSERVATIVE_ORIGINAL_NOTICES_NOT_LINK_SBOM_OR_LEGAL_QUALIFICATION'}
    verify(lambda name: (output / name).read_bytes(), receipt)
    return receipt


def stage(source, output):
    source, output = source.resolve(), output.absolute()
    if output == source or source in output.parents or output in source.parents:
        raise ValueError('PSP notices output must stay outside pristine upstream')
    pin = json.loads((ROOT / 'config/psp-core.proof.json').read_text('utf-8'))
    git = lambda *args: subprocess.check_output(['git', '-C', str(source), *args], text=True).strip()
    revision = git('rev-parse', 'HEAD')
    if revision != pin['commit'] or git('diff', 'HEAD', '--'):
        raise ValueError('PSP original notices require pristine pinned source')
    selected = tracked_notices(source, selection=SELECTION + SUPPLEMENTS)
    if not set(SUPPLEMENTS).issubset({relative for _, relative in selected}):
        raise ValueError('Required PSP inline notice input missing')
    if not any(relative == 'LICENSE.TXT' for _, relative in selected):
        raise ValueError('PPSSPP original GPL/PSPSDK notice missing')
    return stage_files(source, output, selected, revision)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(stage(args.source, args.output), sort_keys=True))
