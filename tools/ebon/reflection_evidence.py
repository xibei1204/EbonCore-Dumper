"""Replay recorded UStruct headers; byte validation does not establish the complete C++ ABI."""
import hashlib
import json
from pathlib import Path

from .sdk_codegen import CodegenError


def replay_alignments(source, state, owners):
    source=Path(source)
    path=source/'reflection_evidence.json'
    expected=(state.get('build') or {}).get('sha256')
    if path.is_file():
        evidence=json.loads(path.read_text(encoding='utf-8'))
        if evidence.get('format')!='EbonReflectionEvidence-v1' or evidence.get('module_sha256')!=expected:
            raise CodegenError('Reflection evidence build/format mismatch')
        size_offset=evidence.get('size_offset')
        offset=evidence.get('alignment_offset')
        width=evidence.get('alignment_width')
        rows=evidence.get('types',[])
    else:
        path=source/'network_evidence.json'
        if not path.is_file():return {}
        # This layout is restricted to the existing version-bound target profile.
        profile_path=Path(__file__).resolve().parents[2]/'profiles'/'nte-sdk-runtime.json'
        profile=json.loads(profile_path.read_text(encoding='utf-8'))
        if expected!=profile.get('module_sha256'):return {}
        evidence=json.loads(path.read_text(encoding='utf-8'))
        if evidence.get('format')!='EbonNetworkEvidence-v1':raise CodegenError('Invalid legacy reflection header evidence')
        size_offset=88;offset=92;width=2
        rows=evidence.get('classes',[])
    if any(isinstance(v,bool) or not isinstance(v,int) or not 0<=v<=4096 for v in (size_offset,offset)) or width not in (2,4):
        raise CodegenError('Invalid reflection evidence offsets/width')
    types={o['path']:o for o in owners}
    result={}
    for row in rows:
        owner=types.get(row.get('path'))
        if owner is None or owner['kind'] not in {'class','struct'}:continue
        if row.get('address')!=owner.get('address'):raise CodegenError('Reflection header object identity mismatch')
        try:raw=bytes.fromhex(row['bytes'])
        except (ValueError,TypeError,KeyError) as error:raise CodegenError('Invalid reflection header bytes') from error
        if len(raw)<max(size_offset+4,offset+width):raise CodegenError('Truncated reflection header')
        size=int.from_bytes(raw[size_offset:size_offset+4],'little',signed=True)
        alignment=int.from_bytes(raw[offset:offset+width],'little')
        if size!=owner['size']:raise CodegenError('Reflection header size differs from parsed type: '+owner['path'])
        if alignment not in (1,2,4,8,16,32,64) or (owner['kind']=='struct' and size and size%alignment):continue
        if owner.get('alignment') not in (None,alignment):raise CodegenError('Reflection header alignment differs from parsed type: '+owner['path'])
        previous=result.get(owner['path'])
        if previous and previous['alignment']!=alignment:raise CodegenError('Conflicting alignment evidence')
        result[owner['path']]={'alignment':alignment,'source':str(path.resolve()),
                              'header_sha256':hashlib.sha256(raw).hexdigest(),
                              'offset':offset,'width':width,'status':'header_bytes_replayed'}
        owner['alignment']=alignment
        owner['alignment_evidence']=result[owner['path']]
    return result


def replay_property_targets(source,state,owners):
    path=Path(source)/'reflection_evidence.json'
    if not path.is_file():return []
    evidence=json.loads(path.read_text(encoding='utf-8'))
    if evidence.get('module_sha256')!=(state.get('build') or {}).get('sha256'):
        raise CodegenError('Property evidence build mismatch')
    layout=state.get('field_layout',{})
    offsets={k:layout.get(k,default) for k,default in [('offset',76),('element_size',60),('array_dim',56),('flags',64)]}
    if any(isinstance(v,bool) or not isinstance(v,int) or not 0<=v<=4096 for v in offsets.values()):
        raise CodegenError('Invalid property evidence layout')
    types={o['path']:o for o in owners}
    addresses={o.get('address'):o for o in owners if o.get('address')}
    fields={(o['path'],f['name']):f for o in owners for f in o['fields']}
    changes=[]
    for row in evidence.get('properties',[]):
        field=fields.get((row.get('owner'),row.get('name')))
        if field is None:continue
        if row.get('address')!=field.get('address'):raise CodegenError('Property evidence identity mismatch')
        try:raw=bytes.fromhex(row['bytes'])
        except (KeyError,TypeError,ValueError) as error:raise CodegenError('Invalid property bytes') from error
        def integer(offset,width):
            if len(raw)<offset+width:raise CodegenError('Truncated property evidence')
            return int.from_bytes(raw[offset:offset+width],'little')
        for key in ('offset','element_size','array_dim','flags'):
            if integer(offsets[key],8 if key=='flags' else 4)!=field[key]:raise CodegenError('Property header differs from parsed field: '+row['name'])
        kind=field['type']
        if kind not in {'ObjectProperty','ClassProperty','SoftClassProperty','SoftObjectProperty','WeakObjectProperty',
                         'LazyObjectProperty','InterfaceProperty','StructProperty','EnumProperty','ByteProperty'}:continue
        slot=offsets['offset']+0x2c+(8 if kind in {'ClassProperty','SoftClassProperty','EnumProperty'} else 0)
        target=addresses.get(hex(integer(slot,8)))
        expected='struct' if kind=='StructProperty' else 'enum' if kind in {'EnumProperty','ByteProperty'} else 'class'
        if target is None or target['kind']!=expected:continue
        if expected=='struct' and target['size']!=field['element_size']:continue
        old=field.get('target');new=target['path']
        field['target']=new
        if not isinstance(field.get('detail'),dict):field['detail']={}
        field['detail'].update(target=new,target_offset=slot)
        if old!=new:changes.append({'owner':row['owner'],'field':row['name'],'before':old,'target':new,
                                  'source':str(path.resolve()),'offset':slot,'status':'own_target_slot_replayed'})
    return changes
