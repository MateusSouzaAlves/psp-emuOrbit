"""Stage only tracked PPSSPP assets from the exact product pin, without downloads.

Generated payload and manifest belong to a build directory, never to Git.
No ROM, save, account data or binary from a local proof is an input here.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
# GitHub Pages deployment marker, not a runtime asset. AAPT excludes dotfiles;
# keep the runtime manifest honest instead of requesting an absent package entry.
NON_RUNTIME_METADATA = {'PPSSPP/debugger/.nojekyll'}


def sha(path):
    result = hashlib.sha256()
    with path.open('rb') as source:
        while chunk := source.read(65536):
            result.update(chunk)
    return result.hexdigest()


def valid_name(name):
    return (len(name) <= 240 and name.startswith('PPSSPP/')
            and re.fullmatch(r'[A-Za-z0-9_./ -]+', name) is not None
            and all(part and part not in ('.', '..') and part == part.strip(' ')
                    and not part.endswith('.') for part in name.split('/')))


def tracked_files(repository, prefix='', selection=()):
    """Include pinned gitlinks too; never replace a missing submodule by silence."""
    result = []
    listing = subprocess.check_output(['git', '-C', str(repository), 'ls-files', '--stage', '-z', '--', *selection])
    for record in listing.decode('utf-8').split('\0'):
        if not record:
            continue
        metadata, name = record.split('\t', 1)
        mode, expected, stage_number = metadata.split(' ')
        if stage_number != '0':
            raise ValueError('Unmerged upstream asset')
        if mode == '160000':
            child = repository / name
            if not (child / '.git').exists():
                raise ValueError(f'Initialize pinned assets submodule first: {prefix}{name}')
            actual = subprocess.check_output(['git', '-C', str(child), 'rev-parse', 'HEAD'], text=True).strip()
            if actual != expected or subprocess.check_output(['git', '-C', str(child), 'diff', 'HEAD', '--']):
                raise ValueError('PPSSPP asset submodule pin/source mismatch')
            result.extend(tracked_files(child, prefix + name + '/'))
        elif mode in ('100644', '100755'):
            result.append((repository / name, prefix + name))
        else:
            raise ValueError('Symlink or unsupported upstream asset entry')
    return result


def stage(source, output):
    source, output = source.resolve(), output.absolute()
    pin = json.loads((ROOT / 'config/psp-core.proof.json').read_text('utf-8'))
    revision = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
    if revision != pin['commit']:
        raise ValueError('PPSSPP assets source pin mismatch')
    if subprocess.check_output(['git', '-C', str(source), 'diff', 'HEAD', '--', 'assets']):
        raise ValueError('Tracked upstream assets changed')
    tracked = tracked_files(source, selection=('assets',))
    entries, names, total = [], set(), 0
    for asset, relative in sorted(tracked, key=lambda entry: entry[1]):
        name = 'PPSSPP/' + Path(relative).relative_to('assets').as_posix()
        if name in NON_RUNTIME_METADATA:
            continue
        if not valid_name(name) or name.lower() in names:
            raise ValueError('Unsafe or duplicate PPSSPP asset path')
        if not asset.is_file() or asset.is_symlink() or source not in asset.resolve().parents:
            raise ValueError('PPSSPP assets must be regular tracked files')
        size = asset.stat().st_size
        total += size
        if size > 32 * 1024 * 1024 or total > 128 * 1024 * 1024:
            raise ValueError('PPSSPP assets exceed explicit product bounds')
        names.add(name.lower())
        entries.append((asset, name, sha(asset)))
    if not 0 < len(entries) <= 1024:
        raise ValueError('PPSSPP asset count outside product bound')
    manifest = ''.join(f'{digest}  {name}\n' for _, name, digest in entries).encode('utf-8')
    if len(manifest) > 512 * 1024:
        raise ValueError('PPSSPP asset manifest exceeds bound')
    # Never merge/overwrite an earlier candidate payload or follow its links.
    output.mkdir(parents=True, exist_ok=False)
    for asset, name, digest in entries:
        destination = output / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(asset, destination)
        if sha(destination) != digest or sha(asset) != digest:
            raise ValueError('PPSSPP asset changed during staging')
    (output / 'manifest.sha256').write_bytes(manifest)
    return {'core_pin': revision, 'files': len(entries), 'bytes': total,
            'manifest_sha256': hashlib.sha256(manifest).hexdigest()}


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(stage(args.source, args.output), sort_keys=True))
