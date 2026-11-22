"""Verify the opt-in loader's code prefixes against its retained offline image."""
import argparse
import hashlib
import json
from pathlib import Path
import struct


def verify(image, spec):
    data = Path(image).read_bytes()
    contract = json.loads(Path(spec).read_text(encoding='utf-8'))
    if hashlib.sha256(data).hexdigest() != contract['analysis_sha256']:
        raise ValueError('analysis image identity differs from the reviewed loader contract')
    pe = struct.unpack_from('<I', data, 60)[0]
    if data[:2] != b'MZ' or data[pe:pe + 4] != b'PE\0\0' or struct.unpack_from('<H', data, pe + 4)[0] != 0x8664:
        raise ValueError('expected an AMD64 PE image')
    count = struct.unpack_from('<H', data, pe + 6)[0]
    optional = struct.unpack_from('<H', data, pe + 20)[0]
    sections = [struct.unpack_from('<IIII', data, pe + 24 + optional + index * 40 + 8) for index in range(count)]
    verified = []
    for entry in contract['entries']:
        rva = int(entry['rva'], 0)
        raw = next((data[offset + rva - start:offset + rva - start + 16]
                    for virtual, start, size, offset in sections if 0 <= rva - start <= size - 16), None)
        if raw is None or raw.hex() != entry['prefix_hex'] or hashlib.sha256(raw).hexdigest() != entry['prefix_sha256']:
            raise ValueError('loader entry bytes differ: ' + entry['name'])
        verified.append(dict(name=entry['name'], rva=entry['rva'], prefix_sha256=entry['prefix_sha256']))
    return dict(format='EbonCharacterLoaderImageCheck-v1', success=True, entries=verified, runtime_verified=False)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--spec', type=Path, default=Path(__file__).resolve().parents[1] / 'docs/character-loader.spec.json')
    args = parser.parse_args()
    print(json.dumps(verify(args.image, args.spec), indent=2))
