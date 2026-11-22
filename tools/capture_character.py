"""Launch the opt-in character dumper, then load/export/audit one table-selected character."""
from __future__ import annotations

import argparse
from datetime import datetime
import json
import os
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tools.ebon.character_request import select_character, request_character
from tools.ebon.sdk_codegen import CodegenError
from tools.ebon_cli import inject_and_dump


def latest_source(selector):
    for source in sorted((ROOT / 'out/reflection').glob('*/resources/res_*'),
                         key=lambda path: path.stat().st_mtime, reverse=True):
        try:
            select_character(source, selector)
            return source
        except (CodegenError, OSError, ValueError):
            continue
    raise CodegenError('no finished F10 capture contains this DT_Character row; use --source explicitly')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--character', default='1090')
    parser.add_argument('--source', type=Path, help='source F10 resource directory with DT_Character')
    parser.add_argument('--capture', type=Path, help='reuse an already initialized character-loader capture')
    args = parser.parse_args()
    try:
        source = args.source.resolve() if args.source else latest_source(args.character)
        target = select_character(source, args.character)
        print(f"Character {target['character_id']}: {target['class_path']}", flush=True)
        print(f'Table source: {source}', flush=True)
        capture = args.capture.resolve() if args.capture else ROOT / 'out/reflection' / (
            'character_' + datetime.now().strftime('%Y%m%d_%H%M%S') + '_' + str(os.getpid()))
        if not args.capture:
            build = ROOT / 'build-character-loader'
            code = inject_and_dump(build / 'EbonDebugInject.exe', build / 'EbonDumper.dll', 'HTGame.exe',
                ROOT / 'profiles/nte-ue5-character.ini', capture, None, 900, 24, None, ['--no-elevate'])
            if code:
                return code
        input('Enter the local game scene, then press Enter here to load and capture the selected character: ')
        result = request_character(capture, source, args.character)
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return 0 if result['success'] else 1
    except (CodegenError, OSError, ValueError, EOFError) as error:
        print('error: ' + str(error), file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
