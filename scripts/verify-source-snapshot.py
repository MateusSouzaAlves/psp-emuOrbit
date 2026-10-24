# SPDX-License-Identifier: MIT
"""Verify a published component source snapshot; not a legal or binary-build verdict."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import subprocess


def normalized(data, mode):
    if mode == 'raw':
        return data
    if mode == 'text-crlf-to-lf' and b'\0' not in data:
        return data.replace(b'\r\n', b'\n')
    raise ValueError('Invalid content normalization')


def verify_records(root, records):
    root = root.resolve()
    seen = set()
    for record in records:
        name = record['path']
        relative = PurePosixPath(name)
        if (name in seen or relative.is_absolute() or '\\' in name or ':' in name
                or '..' in relative.parts or not relative.parts or '.git' in relative.parts):
            raise ValueError('Invalid or duplicate source path: ' + name)
        path = root / name
        if path.is_symlink() or not path.is_file() or not path.resolve().is_relative_to(root):
            raise ValueError('Missing or escaping source file: ' + name)
        content = normalized(path.read_bytes(), record['normalization'])
        if (len(content) != record['normalized_bytes']
                or hashlib.sha256(content).hexdigest() != record['sha256']):
            raise ValueError('Source content mismatch: ' + name)
        seen.add(name)
    if not seen:
        raise ValueError('Empty source inventory')
    return seen


def git(root, *arguments):
    return subprocess.check_output(['git', '-C', str(root), *arguments], text=True, encoding='utf-8').strip()


def verify(root, manifest_path):
    root = root.resolve()
    relative = manifest_path.relative_to(root).as_posix()
    manifest = json.loads(manifest_path.read_text('utf-8'))
    admitted = verify_records(root, manifest['files'])
    links = {item['path']: item['commit'] for item in manifest['upstream_gitlinks']}
    observed = {}
    for line in git(root, 'ls-files', '--stage').splitlines():
        metadata, name = line.split('\t', 1)
        mode, revision, stage = metadata.split()
        if stage != '0':
            raise ValueError('Unmerged source input')
        if mode == '160000':
            observed[name] = revision
    if links != observed:
        raise ValueError('Upstream gitlink inventory mismatch')
    candidates = set(git(root, 'ls-files', '--cached', '--others', '--exclude-standard').splitlines())
    outside = candidates - admitted - set(links) - {relative}
    if outside:
        raise ValueError('Outside source snapshot inventory: ' + ', '.join(sorted(outside)))
    return {'status': 'PASS_SOURCE_FILES_AND_GITLINK_REFERENCES', 'source_files': len(admitted),
            'upstream_gitlinks': len(links), 'scope': 'Source content and recorded pins only; not compilation, binary reproduction or combined-app license certification.'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--manifest', default='releases/component-source-2026-10-24/manifest.json')
    args = parser.parse_args()
    root = args.root.resolve()
    path = root / args.manifest
    if not path.resolve().is_relative_to(root):
        raise ValueError('Manifest must be inside the source checkout')
    print(json.dumps(verify(root, path), sort_keys=True))


if __name__ == '__main__':
    main()
