"""Read-only, class-specific coverage reports for captured replication layouts."""
import argparse
import json
from pathlib import Path

from .runtime_mapping import checked_runtime_mapping
from .runtime_network import CapturedMemory, RuntimeReplay


TARGET_BASES = {
    'PlayerState': '/Script/Engine.PlayerState',
    'Character': '/Script/Engine.Character',
    'AbilitySystemComponent': '/Script/GameplayAbilities.AbilitySystemComponent',
    'GameState': '/Script/Engine.GameStateBase',
    'WorldDataLayers': '/Script/Engine.WorldDataLayers',
    'AttributeSet': '/Script/GameplayAbilities.AttributeSet',
}


def build_coverage_report(capture, *, include_records=False):
    """Cross-check existing mappings or replay bytes entirely in memory.

    This deliberately does not create runtime_mapping.json in the capture.
    A complete table means its declared Parents and Cmds were all validated;
    loaded reflection classes and CPF_Net property totals do not establish it.
    """
    capture = Path(capture).resolve()
    state = json.loads((capture / 'dump_state.json').read_text(encoding='utf-8'))
    with (capture / 'types.jsonl').open(encoding='utf-8') as stream:
        owners = [json.loads(line) for line in stream]
    classes = {t['path']: t for t in owners if t['kind'] == 'class'}
    metadata = json.loads((capture / 'network_metadata.json').read_text(encoding='utf-8'))
    active = metadata.get('active_network_objects', [])
    mapping_path = capture / 'runtime_mapping.json'
    memory = None
    verified = dict(layouts=[], caches=[], handles=[], field_indices=[])
    evidence_source = 'no_active_captured_network_memory'
    if active and (capture / 'network_memory.bin').is_file() and (capture / 'network_memory.bin').stat().st_size:
        if mapping_path.is_file():
            raw = json.loads(mapping_path.read_text(encoding='utf-8'))
            evidence_source = 'runtime_mapping.json_revalidated_against_memory'
        elif metadata.get('runtime_scan', {}).get('graph_retained') is False:
            # Focused captures retain validated records and roots, rather than
            # the intermediate traversal graph. Revalidate their bytes directly.
            raw = dict(format='EbonRuntimeMapping-v1', capture=str(capture),
                       module_sha256=state['build']['sha256'],
                       class_net_caches=metadata.get('runtime_cache_records', []),
                       rep_layouts=metadata.get('rep_layouts', []), packet_verified=False)
            evidence_source = 'native_records_revalidated_against_memory'
        else:
            raw = RuntimeReplay(capture).scan()
            evidence_source = 'network_memory_replayed_in_memory'
        verified = checked_runtime_mapping(capture, owners, state['build']['sha256'],
                                           active, raw_mapping=raw)
        memory = CapturedMemory(capture)

    layouts = []
    for layout in verified['layouts']:
        address = int(layout['address'], 0)
        if layout.get('anchor') != 'parent_array_header':
            address += layout['parents_offset']
        _, parent_count = memory.array(address)
        _, command_count = memory.array(address + 16)
        parents = len(layout['parents'])
        commands = len(layout['commands'])
        complete = (not layout.get('partial') and not layout.get('skipped_cmd_indices')
                    and parents == parent_count and commands == command_count)
        layouts.append(dict(
            owner_class=layout['class'], address=layout['address'],
            status='complete' if complete else 'partial',
            parents_observed=parents, parents_declared=parent_count,
            commands_observed=commands, commands_declared=command_count,
            handles_observed=sum('relative_handle' in c for c in layout['commands']),
            skipped_command_indices=layout.get('skipped_cmd_indices', [])))

    def belongs(path, base):
        seen = set()
        while path and path not in seen:
            if path == base:
                return True
            seen.add(path)
            path = classes.get(path, {}).get('super')
        return False

    groups = {}
    for name, base in TARGET_BASES.items():
        matched = sorted(path for path in classes if belongs(path, base))
        tables = [l for l in layouts if l['owner_class'] in matched]
        owner_records = []
        for path in matched:
            owned = [l for l in tables if l['owner_class'] == path]
            status = ('complete' if any(l['status'] == 'complete' for l in owned)
                      else 'partial' if owned else 'not_observed')
            owner_records.append(dict(
                owner_class=path, reflection_complete=classes[path].get('complete') is not False,
                layout_status=status, layout_addresses=[l['address'] for l in owned]))
        groups[name] = dict(
            base_class=base, loaded_owner_classes=len(matched),
            owner_classes_with_layouts=len({l['owner_class'] for l in tables}),
            complete_layouts=sum(l['status'] == 'complete' for l in tables),
            partial_layouts=sum(l['status'] == 'partial' for l in tables),
            owners=owner_records, layouts=tables)

    report = dict(
        format='EbonNetworkCoverage-v1', capture=str(capture),
        module_sha256=state['build']['sha256'],
        reflection_capture_success=bool(state.get('success')),
        evidence_source=evidence_source,
        runtime_scan=metadata.get('runtime_scan', {}),
        summary=dict(
            complete_layouts=sum(l['status'] == 'complete' for l in layouts),
            partial_layouts=sum(l['status'] == 'partial' for l in layouts),
            class_net_caches=len(verified['caches']),
            field_indices=len(verified['field_indices']),
            replication_handles=len(verified['handles']),
            packet_mapping_verified=False),
        groups=groups, layouts=layouts,
        limitations=[
            'Coverage is restricted to this capture and loaded owner classes.',
            'not_observed does not prove that a class has no replication layout.',
            'Class net cache field indices and CPF_Net totals are not layout coverage.',
            'Complete Parents/Cmds coverage does not establish packet round-trip compatibility.',
        ])
    if include_records:
        report['validated_records'] = verified
    return report


def markdown_report(report):
    rows = ['# Captured replication layout coverage', '',
            f"Capture: `{report['capture']}`", '',
            f"Module SHA-256: `{report['module_sha256']}`", '',
            '| Family | Loaded owner classes | Owners with layouts | Complete tables | Partial tables |',
            '| --- | ---: | ---: | ---: | ---: |']
    for name, group in report['groups'].items():
        rows.append(f"| {name} | {group['loaded_owner_classes']} | "
                    f"{group['owner_classes_with_layouts']} | {group['complete_layouts']} | "
                    f"{group['partial_layouts']} |")
    rows += ['', '## Revalidated tables', '',
             '| Owner class | Address | Status | Parents observed/declared | Cmds observed/declared |',
             '| --- | --- | --- | ---: | ---: |']
    for layout in report['layouts']:
        rows.append(f"| {layout['owner_class']} | {layout['address']} | {layout['status']} | "
                    f"{layout['parents_observed']}/{layout['parents_declared']} | "
                    f"{layout['commands_observed']}/{layout['commands_declared']} |")
    rows += ['', '## Scan evidence and limits', '',
             '```json', json.dumps(report['runtime_scan'], indent=2), '```', '']
    rows.extend('- ' + text for text in report['limitations'])
    rows += ['', 'The JSON report lists every matching concrete owner class and its layout status.', '']
    return '\n'.join(rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--capture', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True, help='New report directory outside the capture')
    args = parser.parse_args()
    if args.out.resolve().is_relative_to(args.capture.resolve()):
        parser.error('--out must be outside the source capture')
    if args.out.exists():
        parser.error('--out must be a new directory')
    report = build_coverage_report(args.capture)
    args.out.mkdir(parents=True, exist_ok=False)
    (args.out / 'network_coverage.json').write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    (args.out / 'network_coverage.md').write_text(markdown_report(report), encoding='utf-8')
    print(json.dumps(dict(output=str(args.out.resolve()), **report['summary']), indent=2))


if __name__ == '__main__':
    main()
