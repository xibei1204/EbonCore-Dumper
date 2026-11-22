"""Request a bounded character load from an already initialized opt-in dumper."""
from __future__ import annotations

import hashlib
import json
import math
import os
from pathlib import Path
import time
import uuid

from .character_capture import _body, _reference, audit_character_captures
from .gameplay_audit import _json, _rows
from .sdk_codegen import CodegenError


def _process_alive(pid):
    if type(pid) is not int or pid <= 0:
        return False
    if os.name != 'nt':
        return False
    import ctypes
    from ctypes import wintypes
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    handle = kernel.OpenProcess(0x1000, False, pid)
    if not handle:
        return False
    try:
        status = wintypes.DWORD()
        return bool(kernel.GetExitCodeProcess(handle, ctypes.byref(status))) and status.value == 259
    finally:
        kernel.CloseHandle(handle)


def select_character(source: Path, selector: str) -> dict:
    source = source.resolve()
    state = _json(source / 'resource_state.json')
    if state.get('status') in {'initializing', 'discovering', 'collecting', 'writing'}:
        raise CodegenError('source character table is still being captured')
    targets = []
    for entry in _rows(source / 'resource_manifest.jsonl'):
        if not entry.get('object', '').endswith('/DT_Character.DT_Character'):
            continue
        header, rows = _body(source, entry)
        if header is None:
            continue
        for row in rows:
            element = row.get('values', {}).get('ElementData', {})
            if element.get('_type') != '/Script/HTGame.CharacterItemData':
                continue
            path = _reference(element.get('value', {}).get('CharacterActorClass'))
            if not path or selector not in (row.get('row'), path, path.rsplit('.', 1)[-1].removesuffix('_C')):
                continue
            package, name = path.split('.', 1)
            if not package.startswith('/Game/Blueprints/Character/Player/') or name != package.rsplit('/', 1)[-1] + '_C':
                raise CodegenError('selected table reference is not an exact player blueprint class')
            if len(path) > 512 or any(not (c.isascii() and (c.isalnum() or c in '/_.')) for c in path):
                raise CodegenError('invalid player class path')
            with (source / entry['file']).open('rb') as stream:
                digest = hashlib.file_digest(stream, 'sha256').hexdigest()
            targets.append(dict(character_id=row['row'], class_path=path, source=str(source),
                                table_file=entry['file'], table_sha256=digest, build_sha256=state.get('build_sha256')))
    if len(targets) != 1:
        raise CodegenError('character selector requires one exact DT_Character row in the source capture')
    return targets[0]


def request_character(capture: Path, source: Path, selector: str, *, timeout=1200.0) -> dict:
    if not math.isfinite(timeout) or not 1 <= timeout <= 3600:
        raise CodegenError('character capture timeout must be in 1..3600 seconds')
    capture = capture.resolve()
    target = select_character(source, selector)
    ready = _json(capture / 'character_loader_ready.json')
    state = _json(capture / 'dump_state.json')
    if ready.get('format') != 'EbonCharacterLoaderReady-v1' or ready.get('enabled') is not True:
        raise CodegenError('this capture was not started with the character-loading profile')
    if ready.get('build_sha256') != target['build_sha256'] or state.get('build', {}).get('sha256') != target['build_sha256']:
        raise CodegenError('source character table and target process belong to different builds')
    if not _process_alive(ready.get('process_id')):
        raise CodegenError('the character-loader process has exited or is unavailable; restart its capture entry')
    identifier = uuid.uuid4().hex
    request = capture / 'character_capture.request'
    temporary = capture / ('.character-' + identifier + '.tmp')
    text = f"request_id={identifier}\nclass_path={target['class_path']}\nbuild_sha256={target['build_sha256']}\n"
    try:
        temporary.write_text(text, encoding='utf-8', newline='\n')
        # Publish a completely written file without replacing another request.
        os.link(temporary, request)
    except FileExistsError as error:
        raise CodegenError('another character capture request is already pending') from error
    finally:
        temporary.unlink(missing_ok=True)
    job = capture / 'characters' / ('char_' + identifier)
    print(f"Requested {target['character_id']}: {target['class_path']}", flush=True)
    print(f'Progress and results: {job}', flush=True)
    until = time.monotonic() + timeout
    last = None
    while time.monotonic() < until:
        if not _process_alive(ready.get('process_id')):
            raise CodegenError(f'character-loader process exited during capture; inspect {job}')
        try:
            progress = _json(job / 'character_load_state.json')
        except (FileNotFoundError, PermissionError, json.JSONDecodeError):
            time.sleep(0.25)
            continue
        phase = progress.get('status')
        if phase != last:
            print('Character capture: ' + str(phase), flush=True)
            last = phase
        if phase == 'failed':
            result = dict(format='EbonCharacterRequestResult-v1', success=False, target=target,
                          load=progress, error=progress.get('error'), job=str(job), client_verified=False)
            with (job / 'character_audit.json').open('x', encoding='utf-8', newline='\n') as stream:
                json.dump(result, stream, ensure_ascii=False, indent=2)
                stream.write('\n')
            return result
        if phase == 'export_finished':
            audit = audit_character_captures([job / 'resources/res_1'], target['character_id'])
            result = dict(format='EbonCharacterRequestResult-v1', success=audit['success'], target=target,
                          load=progress, audit=audit, job=str(job), client_verified=False)
            with (job / 'character_audit.json').open('x', encoding='utf-8', newline='\n') as stream:
                json.dump(result, stream, ensure_ascii=False, indent=2)
                stream.write('\n')
            return result
        time.sleep(0.25)
    raise CodegenError(f'character capture did not finish within {timeout:g}s; inspect {job}. A running engine load is never killed.')
