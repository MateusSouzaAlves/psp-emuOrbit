"""Read-only PSP native payload checks shared by product build and SDK probes."""
import hashlib
from pathlib import Path
import re
import struct


def sha_bytes(data):
    return hashlib.sha256(data).hexdigest()


def sha(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as source:
        while chunk := source.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def allocated(path):
    data = Path(path).read_bytes()
    if data[:6] != b'\x7fELF\x02\x01':
        raise ValueError('ELF64 little endian required')
    header = struct.unpack_from('<16sHHIQQQIHHHHHH', data)
    if header[2] != 183:
        raise ValueError('ARM64 required')
    sections = [struct.unpack_from('<IIQQQQIIQQ', data, header[6] + i * header[11]) for i in range(header[12])]
    strings = sections[header[13]]
    names = data[strings[4]:strings[4] + strings[5]]
    result = {}
    for section in sections:
        if section[2] & 2:
            name = names[section[0]:].split(b'\0', 1)[0].decode()
            result[name] = (section[1], section[2], section[3], section[5],
                            sha_bytes(data[section[4]:section[4] + section[5]]) if section[1] != 8 else None)
    for index in range(header[10]):
        segment = struct.unpack_from('<IIQQQQQQ', data, header[5] + index * header[9])
        if segment[0] == 1 and segment[7] < 16384:
            raise ValueError('16 KiB load alignment required')
    return result


def verify_worker_abi(source):
    header = (source / 'libretro/LibretroGraphicsContext.h').read_text('utf-8')
    enum = re.search(r'enum class EmuThreadState\s*\{([^}]+)\}', header)
    expected = ['DISABLED', 'START_REQUESTED', 'RUNNING', 'PAUSE_REQUESTED', 'PAUSED', 'QUIT_REQUESTED', 'STOPPED']
    if not enum or [value.strip() for value in enum[1].split(',') if value.strip()] != expected:
        raise ValueError('Pinned worker-state ABI changed; review adapter instead of guessing ordinals')
