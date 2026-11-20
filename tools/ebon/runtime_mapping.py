"""Validate captured network cache/replication records before SDK promotion."""
import json
from pathlib import Path
import struct

from .sdk_codegen import CodegenError
from .network_fields import network_fields


def checked_runtime_mapping(source: Path, owners: list[dict], expected_sha: str, active: list[dict], *, raw_mapping=None):
    path=source/'runtime_mapping.json'
    if raw_mapping is not None:
        raw=raw_mapping
    elif not path.is_file():
        if active and (source/'network_memory.json').is_file() and (source/'network_memory.bin').stat().st_size:
            from .runtime_network import RuntimeReplay
            raw=RuntimeReplay(source).scan()
            path.write_text(json.dumps(raw,indent=2)+'\n',encoding='utf-8')
        else:return dict(caches=[],layouts=[],field_indices=[],handles=[],parents=[],packet_verified=False)
    else:raw=json.loads(path.read_text(encoding='utf-8'))
    if raw.get('format')!='EbonRuntimeMapping-v1' or raw.get('module_sha256')!=expected_sha:
        raise CodegenError('Runtime mapping module SHA-256/format mismatch')
    if Path(raw.get('capture','')).resolve()!=source.resolve():raise CodegenError('Runtime mapping capture identity mismatch')
    if not active:raise CodegenError('Runtime mapping requires a captured active network object')
    from .runtime_network import CapturedMemory
    memory=CapturedMemory(source)
    if not any(int(o['address'],0) in {b[0] for b in memory.blocks} for o in active):
        raise CodegenError('Runtime mapping memory is not rooted in the captured network objects')
    types={t['path']:t for t in owners};_,fields=network_fields(owners);indices={}
    with (source/'objects.jsonl').open(encoding='utf-8') as stream:
        for line in stream:
            obj=json.loads(line);indices[obj['index']]=obj
    def block(encoded,address=None,size=None):
        try:data=bytes.fromhex(encoded)
        except (TypeError,ValueError) as e:raise CodegenError('Runtime mapping is missing raw record bytes') from e
        if size is not None and len(data)!=size:raise CodegenError('Runtime mapping raw record size mismatch')
        if address is not None:
            try:actual=memory.read(address,len(data))
            except KeyError as e:raise CodegenError('Runtime mapping raw bytes are outside captured memory') from e
            if actual!=data:raise CodegenError('Runtime mapping record bytes differ from captured memory')
        return data

    def belong(owner,cls):
        seen=set()
        while cls and cls not in seen:
            if owner==cls:return True
            seen.add(cls);cls=types.get(cls,{}).get('super')
        return False

    caches=[];layouts=[];field_indices=[];handles=[];parent_mappings=[]
    for cache in raw.get('class_net_caches',[]):
        cls=cache['class'];address=int(cache['address'],0)
        header=block(cache.get('header_bytes'),address,48)
        base,=struct.unpack_from('<i',header,0);weak_index,serial=struct.unpack_from('<ii',header,16)
        data,count,capacity=struct.unpack_from('<Qii',header,32)
        if cache.get('validation')!='runtime_cross_checked' or indices.get(weak_index,{}).get('path')!=cls or not 0<serial<0x40000000 or not 0<=base<=1000000 or count!=len(cache['entries']) or not 0<=count<=8192 or not count<=capacity<=65536 or base+count>1000000:
            raise CodegenError('Runtime class cache header failed cross-check')
        parent_pointer,=struct.unpack_from('<Q',header,8)
        if parent_pointer:
            parent_header=memory.read(parent_pointer,48)
            parent_base,=struct.unpack_from('<i',parent_header)
            parent_index,parent_serial=struct.unpack_from('<ii',parent_header,16)
            parent_count,=struct.unpack_from('<i',parent_header,40)
            if (indices.get(parent_index,{}).get('path')!=types.get(cls,{}).get('super') or
                    not 0<parent_serial<0x40000000 or not 0<=parent_count<=8192 or parent_base<0 or parent_base+parent_count!=base):
                raise CodegenError('Runtime class cache parent range failed cross-check')
        elif base!=0:
            raise CodegenError('Runtime root class cache has a nonzero field base')
        for key,expected in (('field_count',count),('field_index_start',base),('field_index_end_exclusive',base+count)):
            if key in cache and cache[key]!=expected:raise CodegenError('Runtime class cache index boundary mismatch')
        stride=cache['field_stride']
        if stride not in (16,24):raise CodegenError('Unsupported field cache stride')
        for position,entry in enumerate(cache['entries']):
            body=block(entry.get('record_bytes'),data+position*stride,stride)
            pointer,index,checksum=struct.unpack_from('<QII',body)
            field=fields.get(entry['path'])
            if not field or pointer&~1!=field['address'] or field['owner']!=cls or index!=base+position or index!=entry['field_net_index'] or checksum!=entry['checksum']:
                raise CodegenError('Runtime field cache entry failed cross-check')
            incompatible=bool(body[16]) if stride==24 else False
            if (stride==24 and body[16]>1) or entry.get('incompatible',incompatible)!=incompatible:
                raise CodegenError('Runtime field cache compatibility flag mismatch')
            field_indices.append(dict(class_=cls,path=entry['path'],field_net_index=index,checksum=checksum,
                                      cache_address=cache['address'],record_address=hex(data+position*stride),rpc=field['rpc'],incompatible=incompatible))
        caches.append(cache)
    for layout in raw.get('rep_layouts',[]):
        cls=layout['class'];address=int(layout['address'],0)
        if layout.get('validation')!='runtime_cross_checked':raise CodegenError('Unverified replication layout')
        owner_pointer=int(layout.get('owner_pointer_address','0'),0)
        if cls not in types or memory.u64(owner_pointer)!=int(types[cls]['address'],0):raise CodegenError('Replication layout owner pointer failed cross-check')
        if owner_pointer-address not in (56,64,72,80,96,112) and layout.get('anchor')=='parent_array_header':
            raise CodegenError('Replication layout owner location is inconsistent')
        if layout.get('anchor')=='parent_array_header':
            head=block(layout.get('array_header_bytes'),address,32)
        else:
            head=block(layout.get('header_bytes'),address,512)
            offset=layout['parents_offset'];head=head[offset:offset+32]
        parents_data,parent_count,_=struct.unpack_from('<Qii',head)
        commands_data,command_count,_=struct.unpack_from('<Qii',head,16)
        ps,cs=layout['parent_stride'],layout['command_stride'];ranges=layout.get('parent_ranges_offset',24)
        if ps not in (48,56,64,72) or cs not in (32,40) or ranges not in (24,28):raise CodegenError('Unsupported replication command layout')
        parents=layout['parents']
        if not parents or len(parents)>parent_count:raise CodegenError('Invalid replication parent count')
        for i,parent in enumerate(parents):
            body=block(parent.get('record_bytes'),parents_data+i*ps,ps);field=fields.get(parent['path'])
            pointer,=struct.unpack_from('<Q',body);array,=struct.unpack_from('<i',body,16);start,end=struct.unpack_from('<HH',body,ranges)
            if not field or pointer&~1!=field['address'] or field['rpc'] or not belong(field['owner'],cls) or not 0<=array<field['array_dim'] or start>end or end>command_count:
                raise CodegenError('Replication parent field/command range failed cross-check')
            if parent['parent_index']!=i or parent['array_index']!=array or (parent['cmd_start'],parent['cmd_end'])!=(start,end) or parent['lifetime_condition']!=body[32]:
                raise CodegenError('Replication parent metadata differs from raw bytes')
            parent_mappings.append(dict(class_=cls,path=parent['path'],parent_index=i,array_index=array,lifetime_condition=body[32]))
        observed=set();scopes=[]
        for command in layout['commands']:
            index=command['cmd_index']
            if not 0<=index<command_count or index in observed:raise CodegenError('Invalid/duplicate replication command index')
            observed.add(index);body=block(command.get('record_bytes'),commands_data+index*cs,cs)
            kind=body[28]
            if kind!=command['type']:raise CodegenError('Replication command type differs from raw bytes')
            while scopes and index>=scopes[-1][0]:scopes.pop()
            if kind==1:continue
            pointer,=struct.unpack_from('<Q',body);offset,=struct.unpack_from('<i',body,12)
            handle,parent=struct.unpack_from('<HH',body,20);end,=struct.unpack_from('<H',body,8)
            field=fields.get(command['path'])
            if not field or pointer&~1!=field['address'] or parent>=len(parents) or handle<1 or offset<0 or kind>48:
                raise CodegenError('Replication handle field/parent failed cross-check')
            parent_entry=parents[parent];top=fields[parent_entry['path']]
            if not parent_entry['cmd_start']<=index<parent_entry['cmd_end']:raise CodegenError('Replication command lies outside its parent range')
            if top['address']==field['address'] and offset!=field['offset']+parent_entry['array_index']*field['element_size']:
                raise CodegenError('Replication command object offset mismatch')
            if (command['relative_handle'],command['parent_index'],command['offset'])!=(handle,parent,offset) or command['parent_path']!=parent_entry['path']:
                raise CodegenError('Replication handle metadata differs from captured bytes')
            handles.append(dict(class_=cls,path=command['path'],parent_path=parent_entry['path'],relative_handle=handle,
                                cmd_index=index,parent_index=parent,scope_cmd_index=scopes[-1][1] if scopes else -1,
                                offset=offset,command_type=kind,checksum=struct.unpack_from('<I',body,24)[0],
                                layout_address=layout['address'],record_address=hex(commands_data+index*cs)))
            if kind==0:
                if not index<end<=command_count:raise CodegenError('Invalid dynamic array end command')
                scopes.append((end,index))
        if len(observed)<command_count and not layout.get('partial'):raise CodegenError('Replication layout command coverage was overstated')
        skipped=set(layout.get('skipped_cmd_indices',[]))
        if skipped and (skipped & observed or observed|skipped!=set(range(command_count))):
            raise CodegenError('Partial replication layout coverage is inconsistent')
        layouts.append(layout)
    for collection in (field_indices,handles,parent_mappings):
        for row in collection:row['class']=row.pop('class_')
    return dict(caches=caches,layouts=layouts,field_indices=field_indices,handles=handles,parents=parent_mappings,
                packet_verified=False,memory_file='network_memory.bin')
