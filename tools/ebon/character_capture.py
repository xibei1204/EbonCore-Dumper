"""Check one character's captured configuration and exact replication layouts."""
from __future__ import annotations

import hashlib
from pathlib import Path

from .gameplay_audit import _json, _rows
from .network_coverage import build_coverage_report
from .sdk_codegen import CodegenError


def _body(capture, entry):
    relative = entry.get('file')
    if not isinstance(relative, str) or Path(relative).is_absolute():
        raise CodegenError('character body must use a relative path')
    path = (capture / relative).resolve()
    if not path.is_relative_to(capture):
        raise CodegenError('character body path escapes the capture')
    if not path.is_file():
        return None, None
    rows = iter(_rows(path))
    header = next(rows, None)
    if not header or header.get('object') != entry['object']:
        raise CodegenError('character body identity differs from its manifest')
    return header, rows


def _reference(value):
    if not isinstance(value, dict):
        return None
    path = value.get('asset_path') or value.get('object')
    return path if isinstance(path, str) and path.startswith('/Game/') else None


def _field(values, name):
    while isinstance(values, dict):
        if name in values:
            return values[name]
        values = values.get('_super')
    return None


def _configuration(capture, entry, header):
    missing, abilities = [], []
    for field in ('GrantedAbilities', 'PassiveAbilities'):
        values = _field(header.get('values'), field)
        if not isinstance(values, list):
            missing.append(field + '_not_observed')
            continue
        for index, value in enumerate(values):
            path = _reference(value.get('AbilityClass')) if isinstance(value, dict) else None
            input_id = value.get('InputID') if isinstance(value, dict) else None
            if isinstance(input_id, dict):
                input_id = input_id.get('value')
            if not path or type(input_id) is not int or not 0 <= input_id <= 0xffffffff:
                missing.append(f'{field}[{index}]_unresolved')
            else:
                abilities.append(dict(field=field, ability_class=path, input_id=input_id))
    weapons = _field(header.get('values'), 'DefaultWeaponList')
    if not isinstance(weapons, list) or any(_reference(value) is None for value in weapons):
        missing.append('DefaultWeaponList_not_observed')
    path = capture / entry['file']
    with path.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    return dict(object=entry['object'], file=entry['file'], sha256=digest,
                is_default=header.get('is_default') is True, kind=header.get('kind'),
                abilities=abilities, weapon_classes=[_reference(w) for w in weapons or []]
                if isinstance(weapons, list) else [], missing=missing, complete=not missing)


def audit_character_captures(captures, character, *, include_network=True):
    if not captures or len(captures) > 64 or not isinstance(character, str) or not character.strip():
        raise CodegenError('character audit requires a character and 1..64 F10 captures')
    sources = list(dict.fromkeys(Path(c).resolve() for c in captures))
    states = [_json(c / 'resource_state.json') for c in sources]
    for state in states:
        if state.get('status') in {'initializing', 'discovering', 'collecting', 'writing'}:
            raise CodegenError('character capture is still running')
    builds = {state.get('build_sha256') for state in states}
    if len(builds) != 1 or not isinstance(next(iter(builds)), str) or not next(iter(builds)):
        raise CodegenError('character audit requires a single recorded build identity')
    observations = []
    for capture, state in zip(sources, states):
        entries = list(_rows(capture / 'resource_manifest.jsonl'))
        targets = set()
        for entry in entries:
            if not entry.get('object', '').endswith('/DT_Character.DT_Character'):
                continue
            header, rows = _body(capture, entry)
            if header is None:
                continue
            for row in rows:
                element = row.get('values', {}).get('ElementData', {})
                if element.get('_type') != '/Script/HTGame.CharacterItemData':
                    continue
                path = _reference(element.get('value', {}).get('CharacterActorClass'))
                if path and character in (row.get('row'), path, path.rsplit('.', 1)[-1].removesuffix('_C')):
                    targets.add(path)
        if len(targets) > 1:
            raise CodegenError('character selector matches conflicting table classes')
        target = next(iter(targets), None)
        reflection = capture / 'reflection'
        reflected_state = reflection / 'dump_state.json'
        if reflected_state.is_file() and _json(reflected_state).get('build', {}).get('sha256') != state['build_sha256']:
            raise CodegenError('character and reflection evidence build identities differ')
        type_path = reflection / 'types.jsonl'
        classes = {row['path'] for row in _rows(type_path) if row.get('kind') == 'class'} if type_path.is_file() else None
        configurations = []
        if target:
            for entry in entries:
                # Runtime actor paths may omit the character key; select them
                # by their manifest class, then verify the full body class.
                if entry.get('class') not in (target, target.rsplit('.', 1)[-1]):
                    continue
                header, rows = _body(capture, entry)
                if header and header.get('class') == target:
                    if next(rows, None) is not None:
                        raise CodegenError('character object body contains extra records')
                    configurations.append(_configuration(capture, entry, header))
        network = None
        if include_network and (reflection / 'network_metadata.json').is_file():
            network = build_coverage_report(reflection)
            if network['module_sha256'] != state['build_sha256']:
                raise CodegenError('character and network evidence build identities differ')
        layouts = network['layouts'] if network else []
        exact = [layout for layout in layouts if layout['owner_class'] == target]
        layout_complete = any(layout['status'] == 'complete' for layout in exact)
        configured = [body for body in configurations if body['complete']]
        missing = []
        if not target:
            missing.append('character_table_entry_not_observed')
        if classes is None:
            missing.append('reflection_types_not_observed')
        elif target and target not in classes:
            missing.append('generated_class_not_loaded')
        if not configurations:
            missing.append('character_body_not_observed')
        elif not configured:
            missing.append('character_configuration_incomplete')
        if not layout_complete:
            missing.append('exact_character_layout_not_observed' if include_network else 'network_not_checked')
        weapon_classes = sorted({weapon for body in configured for weapon in body['weapon_classes']})
        weapon_layouts = [layout for layout in layouts if layout['owner_class'] in weapon_classes]
        # Each configuration is evaluated on its own, never by joining partial
        # default/runtime bodies or observations from different captures.
        supported = [body for body in configured if all(any(layout['owner_class'] == weapon
                     and layout['status'] == 'complete' for layout in weapon_layouts) for weapon in body['weapon_classes'])]
        if include_network and configured and not supported:
            missing.append('exact_weapon_layout_not_observed')
        if state.get('success') is not True:
            missing.append('source_capture_failed')
        observations.append(dict(capture=str(capture), character_class=target,
            class_loaded=target in classes if target and classes is not None else None,
            configurations=configurations, configuration_observed=bool(configured),
            character_layouts=exact, weapon_layouts=weapon_layouts, missing=missing,
            evidence_complete=not missing))
    return dict(format='EbonCharacterCaptureAudit-v1', character=character, build_sha256=next(iter(builds)),
        success=any(row['evidence_complete'] for row in observations), captures=observations,
        client_verified=False, limitations=[
            'Only loaded objects are read; inventory rows, portraits and meshes do not establish a generated player class.',
            'Required evidence must coexist in one capture; partial configurations and layouts are not spliced.',
            'Configuration and revalidated layout bytes do not establish wire compatibility or successful character switching.'])
