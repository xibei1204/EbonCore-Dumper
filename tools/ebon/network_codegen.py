"""Export separate reflection IDs, class table positions and observed wire IDs."""
import json
from pathlib import Path

from .sdk_codegen import CodegenError, _cpp_string
from .runtime_mapping import checked_runtime_mapping
from .network_version import checked_network_version


def _number(value):
    return int(value, 0) if isinstance(value, str) else int(value or 0)


def _optional(value, label, maximum=65535):
    if value is None:
        return None
    if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= maximum:
        raise CodegenError(f'{label}: invalid network metadata integer')
    return value


def generate_network_files(source: Path, owners: list[dict], verified_owners=None) -> tuple[dict, dict]:
    rpcs=[];properties=[]
    for owner in owners:
        flags=_number(owner.get('flags'))
        if owner['kind']=='function' and flags & 0x40:
            direction='server' if flags & 0x200000 else 'client' if flags & 0x1000000 else 'multicast' if flags & 0x4000 else 'unspecified'
            rpcs.append(dict(path=owner['path'],flags=hex(flags),direction=direction,reliable=bool(flags & 0x80),
                             rpc_id=_optional(owner.get('rpc_id'),owner['path']+'.rpc_id'),
                             rpc_response_id=_optional(owner.get('rpc_response_id'),owner['path']+'.rpc_response_id'),
                             field_net_index=None))
        if owner['kind']=='class':
            for field in owner['fields']:
                if not _number(field.get('flags')) & 0x20: continue
                properties.append(dict(owner=owner['path'],property=field['name'],flags=hex(_number(field['flags'])),
                                       rep_index=_optional(field.get('rep_index'),owner['path']+'.'+field['name']+'.rep_index'),
                                       blueprint_replication_condition=_optional(field.get('rep_condition'),owner['path']+'.'+field['name']+'.rep_condition',255),
                                       rep_notify=field.get('rep_notify'),lifetime_condition=None,rep_layout_handle=None))
    tables=[];active=[];coverage='not_observed';layout_offsets={};candidates=[]
    path=source/'network_metadata.json'
    if path.is_file():
        raw=json.loads(path.read_text(encoding='utf-8'))
        if raw.get('format')!='EbonNetwork-v1': raise CodegenError('Unsupported network metadata format')
        if len(raw.get('rpcs',[]))!=len(rpcs) or len(raw.get('properties',[]))!=len(properties):
            raise CodegenError('Network metadata row counts do not match reflection')
        expected_rpcs={r['path']:r for r in rpcs}
        seen=set()
        for row in raw['rpcs']:
            expected=expected_rpcs.get(row['path'])
            if expected is None or row['path'] in seen:raise CodegenError('Unknown/duplicate RPC network record')
            seen.add(row['path'])
            if row.get('rpc_id')!=expected['rpc_id'] or row.get('rpc_response_id')!=expected['rpc_response_id']:
                raise CodegenError('RPC metadata differs from captured UFunction fields')
            if row.get('field_net_index') is not None:
                raise CodegenError('An observed FClassNetCache decoder is required for wire field indices')
        expected_props={(p['owner'],p['property']):p for p in properties};seen=set()
        for row in raw['properties']:
            key=(row['owner'],row['property']);expected=expected_props.get(key)
            if expected is None or key in seen:raise CodegenError('Unknown/duplicate replicated property record')
            seen.add(key)
            if row.get('rep_index')!=expected['rep_index']:raise CodegenError('Replication index differs from captured FProperty')
            if row.get('rep_layout_handle') is not None:
                raise CodegenError('An observed FRepLayout decoder is required for replication handles')
        tables=raw.get('class_tables',[]);active=raw.get('active_network_objects',[]);layout_offsets=raw.get('layout_offsets',{})
        candidates=raw.get('runtime_cache_candidates',[])
        for table in tables:
            if table['kind']=='class_net_fields':
                for position,row in enumerate(table['entries']):
                    if row.get('position')!=position or row.get('path') not in expected_rpcs:
                        raise CodegenError('Invalid class NetFields table')
            elif table['kind']=='class_reps':
                for position,row in enumerate(table['entries']):
                    p=expected_props.get((row['owner'],row['property']))
                    if p is None or p['rep_index'] is None or p['rep_index']+row.get('array_index',-1)!=position or row['rep_index']!=position:
                        raise CodegenError('Invalid class replication record')
            else:raise CodegenError('Unknown class network table kind')
    # A scene snapshot can contain unloading Blueprint metadata. Do not let
    # corrupt flags produce invented RPC IDs merely because FUNC_Net is set.
    by_path={o['path']:o for o in owners}
    baseline={o['path']:o for o in (verified_owners or [])}
    excluded=[]
    accepted=[]
    for row in rpcs:
        owner=by_path[row['path']];reference=baseline.get(row['path'])
        reason=None
        if owner.get('complete') is False:reason='function layout failed in scene snapshot'
        elif reference and _number(reference.get('flags'))!=_number(owner.get('flags')):reason='function flags changed from verified layout capture'
        elif sum(bool(_number(owner.get('flags'))&flag) for flag in (0x200000,0x1000000,0x4000))!=1:reason='RPC direction flags are inconsistent'
        if reason:excluded.append(dict(path=row['path'],reason=reason))
        else:accepted.append(row)
    rpcs=accepted
    accepted=[]
    for row in properties:
        owner=by_path[row['owner']];reference=baseline.get(row['owner'])
        field=next(f for f in owner['fields'] if f['name']==row['property'])
        before=next((f for f in reference['fields'] if f['name']==row['property']),None) if reference else None
        reason=None
        if owner.get('complete') is False and before is None:reason='property owner layout failed in scene snapshot'
        elif before and any(field.get(k)!=before.get(k) for k in ('offset','element_size','array_dim','type')):reason='property layout changed from verified capture'
        elif before and _number(field.get('flags'))!=_number(before.get('flags')):reason='property flags changed from verified capture'
        if reason:excluded.append(dict(path=row['owner']+'.'+row['property'],reason=reason))
        else:accepted.append(row)
    properties=accepted
    rpc_paths={row['path'] for row in rpcs};prop_keys={(p['owner'],p['property']) for p in properties}
    tables=[t for t in tables if all((row['path'] in rpc_paths) if t['kind']=='class_net_fields' else
                                    ((row['owner'],row['property']) in prop_keys) for row in t['entries'])]
    function_paths={o['path'] for o in owners if o['kind']=='function' and o.get('complete') is not False}
    classes={o['path']:o for o in owners if o['kind']=='class'}
    rejected_notify=[]
    for p in properties:
        notify=p['rep_notify']
        if not notify:continue
        current=classes.get(p['owner']);seen=set();validated=False
        while current and current['path'] not in seen:
            seen.add(current['path'])
            if current['path']+'.'+notify in function_paths:validated=True;break
            current=classes.get(current.get('super'))
        if not validated:
            rejected_notify.append(dict(owner=p['owner'],property=p['property'],raw_name=notify))
            p['rep_notify']=None
    validated_reps={(r['owner'],r['property']) for t in tables if t['kind']=='class_reps' for r in t['entries']}
    for p in properties:p['rep_index_validated']=(p['owner'],p['property']) in validated_reps
    state=json.loads((source/'dump_state.json').read_text(encoding='utf-8'))
    try:version=checked_network_version(source,(state.get('build') or {}).get('sha256'))
    except ValueError as error:raise CodegenError('Invalid NetworkVersion evidence: '+str(error)) from error
    try:runtime=checked_runtime_mapping(source,owners,(state.get('build') or {}).get('sha256'),active)
    except (KeyError,TypeError,ValueError) as error:raise CodegenError('Invalid runtime mapping evidence: '+str(error)) from error
    rpc_index_map={}
    for row in runtime['field_indices']:
        if row['rpc']:rpc_index_map.setdefault(row['path'],set()).add(row['field_net_index'])
    for rpc in rpcs:
        values=rpc_index_map.get(rpc['path'],set())
        if len(values)==1:rpc['field_net_index']=next(iter(values))
    for prop in properties:
        path=prop['owner']+'.'+prop['property']
        matches=[r for r in runtime['handles'] if r['parent_path']==path]
        prop['rep_layout_handles']=matches
        values={r['relative_handle'] for r in matches if r['path']==path and r['scope_cmd_index']==-1}
        if len(values)==1:prop['rep_layout_handle']=next(iter(values))
        conditions={r['lifetime_condition'] for r in runtime['parents'] if r['path']==path}
        if len(conditions)==1:prop['lifetime_condition']=next(iter(conditions))
    observed_indices=len(runtime['field_indices']);observed_handles=len(runtime['handles'])
    if observed_indices or observed_handles:coverage='observed_partial'
    summary=dict(rpc_count=len(rpcs),replicated_property_count=len(properties),
                 rpc_ids_read=sum(r['rpc_id'] is not None for r in rpcs),
                 rpc_ids_nonzero=sum(bool(r['rpc_id']) for r in rpcs),
                 rep_indices_read=sum(p['rep_index'] is not None for p in properties),
                 rep_indices_not_sentinel=sum(p['rep_index'] is not None and p['rep_index']!=65535 for p in properties),
                 rep_indices_validated=sum(p['rep_index_validated'] for p in properties),
                 rep_notify_validated=sum(bool(p['rep_notify']) for p in properties),rep_notify_rejected=len(rejected_notify),
                 class_net_fields_tables=sum(t['kind']=='class_net_fields' for t in tables),
                 class_replication_tables=sum(t['kind']=='class_reps' for t in tables),
                 active_network_objects=len(active),field_net_indices_observed=observed_indices,rep_layout_handles_observed=observed_handles,
                 rpc_field_net_indices_observed=sum(r['field_net_index'] is not None for r in rpcs),
                 class_net_caches_observed=len(runtime['caches']),rep_layouts_observed=len(runtime['layouts']),
                 partial_rep_layouts=sum(bool(l.get('partial')) for l in runtime['layouts']),
                 rep_layout_parent_records_observed=sum(len(l['parents']) for l in runtime['layouts']),
                 rep_layout_parents_declared=sum(l.get('declared_parent_count',len(l['parents'])) for l in runtime['layouts']),
                 rep_layout_commands_validated=sum(len(l['commands']) for l in runtime['layouts']),
                 rep_layout_commands_declared=sum(l.get('declared_command_count',len(l['commands'])) for l in runtime['layouts']),
                 packet_mapping_verified=False,
                 class_net_cache_candidates=len(candidates),
                 excluded_unstable_records=len(excluded),
                  runtime_mapping_status=coverage,
                  local_network_version=version['value'],network_version_verified=version['verified'],
                  network_version_status=version['status'])
    normalized=dict(format='EbonNetwork-v1',summary=summary,rpcs=rpcs,properties=properties,
                    runtime_mapping_status=coverage,
                    class_tables=tables,active_network_objects=active,layout_offsets=layout_offsets,rejected_rep_notify=rejected_notify,runtime_cache_candidates=candidates,excluded_unstable_records=excluded,
                    runtime_mappings=runtime,network_version=version,
                    limitations=['RPCId/RPCResponseId are UFunction service fields, not wire indices.',
                                 'FProperty RepIndex is distinct from FRepLayout handles and class-cache field indices.',
                                 'BlueprintReplicationCondition does not prove a native lifetime condition.',
                                 'Class NetFields positions are recorded in memory order and are not relabelled as wire indices.',
                                 'Runtime mappings cover only the captured caches, classes and command scopes; packet replay and Iris handles are not verified.'])
    version_value=str(version['value'])+'U' if version['verified'] else 'std::nullopt'
    lines=['#pragma once','#include "Basic.hpp"','#include <optional>','namespace SDK::Network {',
           'inline constexpr std::optional<std::uint32_t> LocalNetworkVersion = '+version_value+';',
           'inline constexpr bool NetworkVersionVerified = '+str(version['verified']).lower()+';',
           'enum class ERpcDirection { Unspecified, Server, Client, Multicast };',
           'struct FRpcInfo { std::string_view Path; std::uint32_t Flags; ERpcDirection Direction; bool Reliable; std::int32_t RpcId, RpcResponseId, FieldNetIndex; };',
           'struct FReplicatedPropertyInfo { std::string_view Owner, Name; std::uint64_t Flags; std::int32_t RepIndex, BlueprintCondition; std::string_view RepNotify; std::int32_t RepLayoutHandle; bool RepIndexValidated; };',
           f'inline constexpr std::array<FRpcInfo, {len(rpcs)}> Rpcs{{{{']
    opt=lambda value: str(value) if value is not None else '-1'
    for row in rpcs:
        lines.append('    {'+', '.join([_cpp_string(row['path']),row['flags']+'U','ERpcDirection::'+row['direction'].title(),
                                        str(row['reliable']).lower(),opt(row['rpc_id']),opt(row['rpc_response_id']),opt(row['field_net_index'])])+'},')
    lines+=['}};',f'inline constexpr std::array<FReplicatedPropertyInfo, {len(properties)}> ReplicatedProperties{{{{']
    for row in properties:
        lines.append('    {'+', '.join([_cpp_string(row['owner']),_cpp_string(row['property']),row['flags']+'ULL',
                                        opt(row['rep_index']),opt(row['blueprint_replication_condition']),_cpp_string(row['rep_notify'] or ''),opt(row['rep_layout_handle']),str(row['rep_index_validated']).lower()])+'},')
    lines+=['}};',
            'struct FRuntimeFieldIndex { std::string_view Class, Path; std::uint32_t FieldNetIndex, Checksum; };',
            f'inline constexpr std::array<FRuntimeFieldIndex, {len(runtime["field_indices"])}> RuntimeFieldIndices{{{{']
    for row in runtime['field_indices']:
        lines.append('    {'+', '.join([_cpp_string(row['class']),_cpp_string(row['path']),str(row['field_net_index']),str(row['checksum'])+'U'])+'},')
    lines+=['}};',
            'struct FRuntimeRepHandle { std::string_view Class, Path, ParentPath; std::uint32_t RelativeHandle, CmdIndex, ParentIndex; std::int32_t ScopeCmdIndex; std::uint32_t Offset; };',
            f'inline constexpr std::array<FRuntimeRepHandle, {len(runtime["handles"])}> RuntimeRepHandles{{{{']
    for row in runtime['handles']:
        lines.append('    {'+', '.join([_cpp_string(row['class']),_cpp_string(row['path']),_cpp_string(row['parent_path']),str(row['relative_handle']),str(row['cmd_index']),str(row['parent_index']),str(row['scope_cmd_index']),str(row['offset'])])+'},')
    lines+=['}};',
            'struct FClassRpcField { std::string_view Class, Function; std::uint32_t Position, HeaderOffset; };',
            f'inline constexpr std::array<FClassRpcField, {sum(len(t["entries"]) for t in tables if t["kind"]=="class_net_fields")}> ClassNetFields{{{{']
    for table in tables:
        if table['kind']=='class_net_fields':
            for row in table['entries']:lines.append('    {'+', '.join([_cpp_string(table['class']),_cpp_string(row['path']),str(row['position']),str(table['header_offset'])])+'},')
    lines+=['}};',
            'struct FClassRepRecord { std::string_view Class, DeclaringClass, Property; std::uint32_t RepIndex, ArrayIndex, HeaderOffset; };',
            f'inline constexpr std::array<FClassRepRecord, {sum(len(t["entries"]) for t in tables if t["kind"]=="class_reps")}> ClassRepRecords{{{{']
    for table in tables:
        if table['kind']=='class_reps':
            for row in table['entries']:lines.append('    {'+', '.join([_cpp_string(table['class']),_cpp_string(row['owner']),_cpp_string(row['property']),str(row['rep_index']),str(row['array_index']),str(table['header_offset'])])+'},')
    lines+=['}};',
            'inline constexpr const FRpcInfo* FindRpc(std::string_view path) { for(const auto& rpc:Rpcs) if(rpc.Path==path) return &rpc; return nullptr; }',
            'inline constexpr const FReplicatedPropertyInfo* FindProperty(std::string_view owner, std::string_view name) { for(const auto& field:ReplicatedProperties) if(field.Owner==owner && field.Name==name) return &field; return nullptr; }',
            'inline constexpr const FRuntimeFieldIndex* FindFieldIndex(std::string_view owner, std::string_view path) { for(const auto& field:RuntimeFieldIndices) if(field.Class==owner && field.Path==path) return &field; return nullptr; }',
            'inline constexpr const FRuntimeRepHandle* FindRepHandle(std::string_view owner, std::uint32_t handle, std::int32_t scope=-1) { for(const auto& field:RuntimeRepHandles) if(field.Class==owner && field.RelativeHandle==handle && field.ScopeCmdIndex==scope) return &field; return nullptr; }',
            'inline constexpr bool HasObservedWireMapping = '+str(bool(observed_indices and observed_handles)).lower()+';',
            'inline constexpr bool HasObservedRuntimeMapping = '+str(bool(observed_indices or observed_handles)).lower()+';',
            'inline constexpr bool PacketMappingVerified = false;',
            '// -1 = unavailable. RpcId==0 is a captured zero, not an inferred wire ID.',
            '// RepIndex==65535 is the engine unassigned sentinel; it is never a handle.',
            '}']
    return {'Network.hpp':'\n'.join(lines)+'\n','network_metadata.json':json.dumps(normalized,ensure_ascii=False,indent=2)+'\n'},summary
