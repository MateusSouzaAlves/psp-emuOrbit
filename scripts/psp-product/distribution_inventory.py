"""Closed R3/R4 component/asset-to-notice inventory, not a legal opinion.

Only build metadata, pinned sources and packaged bytes are read. Unknown linked
archives/assets and missing/altered notices refuse qualification; no network,
user data, signing changes or publication are performed here.
"""
import hashlib
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
INVENTORY = ROOT / 'config/psp-distribution-inventory.json'


def sha(data):
    return hashlib.sha256(data).hexdigest()


def load():
    data = json.loads(INVENTORY.read_text('utf-8'))
    pin = json.loads((ROOT / 'config/psp-core.proof.json').read_text('utf-8'))
    if data['schema'] != 1 or data['psp_pin'] != pin['commit']:
        raise ValueError('Distribution inventory pin/schema mismatch')
    ids = [c['id'] for c in data['components']]
    archives = [a for c in data['components'] for a in c['archives']]
    assets = [a['path'] for a in data['assets']]
    if len(ids) != len(set(ids)) or len(archives) != len(set(archives)) or len(assets) != len(set(assets)):
        raise ValueError('Duplicate distribution inventory member')
    if any(not c['license'] or not c['notices'] for c in data['components']):
        raise ValueError('Component lacks license/notice mapping')
    if any(a['component'] not in ids or not a['path'].startswith('PPSSPP/') or '..' in a['path'].split('/') for a in data['assets']):
        raise ValueError('Unmapped/unsafe runtime asset')
    return data


def verify_links(text, data):
    blocks = text.split('build ppsspp_libretro_android.so:', 1)
    if len(blocks) != 2:
        raise ValueError('Actual PSP link target missing')
    lines = blocks[1].split('\n\n', 1)[0].splitlines()
    links = [line for line in lines if line.startswith('  LINK_LIBRARIES =')]
    if len(links) != 1:
        raise ValueError('Actual PSP link command missing/ambiguous')
    actual = re.findall(r'([^ /]+\.a)(?= |$)', links[0])
    expected = [a for c in data['components'] for a in c['archives']]
    if len(actual) != len(set(actual)) or set(actual) != set(expected):
        raise ValueError('PSP linked archives differ from reviewed inventory')
    return sorted(actual)


def build_record(cache):
    data = load()
    archives = verify_links((cache / 'core/build.ninja').read_text('utf-8', errors='replace'), data)
    return {'schema': 1, 'inventory_sha256': sha(INVENTORY.read_bytes()),
            'linked_archives': archives, 'components': len(data['components']),
            'assets': len(data['assets']), 'scope': 'CLOSED_COMPONENT_ASSET_NOTICE_MAPPING_NOT_LEGAL_OPINION'}


def notice_location(relative):
    # Only these checked repo assets are allowed outside the original PSP tree.
    if relative.startswith('upstream/') and '..' not in relative.split('/'):
        return 'assets/licenses/psp/' + relative
    allowed = {'../apache-2.0.txt'} | {
        '../psp-supplemental/' + Path(n['path']).name for n in load()['supplemental_notices']}
    if relative not in allowed:
        raise ValueError('Unreviewed notice location')
    return 'assets/licenses/' + relative[3:]


def verify_package(read, asset_names, expected):
    data = load()
    if expected.get('inventory_sha256') != sha(INVENTORY.read_bytes()):
        raise ValueError('Changed component/asset inventory')
    archives = sorted(a for c in data['components'] for a in c['archives'])
    if expected.get('linked_archives') != archives or expected.get('components') != len(data['components']) or expected.get('assets') != len(data['assets']):
        raise ValueError('Missing actual linked-component inventory receipt')
    if set(asset_names) != {a['path'] for a in data['assets']}:
        raise ValueError('Runtime assets differ from reviewed distribution inventory')
    for component in data['components']:
        for name in component['notices']:
            if not read(notice_location(name)):
                raise ValueError('Required component notice is empty')
    for notice in data['supplemental_notices']:
        path = ROOT / notice['path']
        if sha(path.read_bytes()) != notice['sha256'] or read('assets/licenses/psp-supplemental/' + path.name) != path.read_bytes():
            raise ValueError('Supplemental original notice changed/missing')
    return expected
