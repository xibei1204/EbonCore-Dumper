"""Correlate captured UObject vtable entries with ProcessEvent code semantics.

Reads an operator-supplied PE and reflection capture; never executes the game.
"""
from pathlib import Path
import argparse
import hashlib
import json
import struct
from capstone import Cs, CS_ARCH_X86, CS_MODE_64


def verify(image: Path, capture: Path) -> dict:
    data=image.read_bytes()
    pe=struct.unpack_from('<I',data,0x3c)[0]
    if data[pe:pe+4]!=b'PE\0\0':raise ValueError('Not a PE image')
    sections=struct.unpack_from('<H',data,pe+6)[0]
    optional=struct.unpack_from('<H',data,pe+20)[0]
    base=struct.unpack_from('<Q',data,pe+24+24)[0]
    ranges=[]
    for i in range(sections):
        virtual_size,rva,size,offset=struct.unpack_from('<IIII',data,pe+24+optional+i*40+8)
        ranges.append((rva,max(size,virtual_size),offset,size))
    def code(rva,count):
        for start,size,offset,raw_size in ranges:
            if start<=rva<start+raw_size:return data[offset+rva-start:offset+rva-start+count]
        return b''
    state=json.loads((capture/'dump_state.json').read_text(encoding='utf-8'))
    if not state.get('success'):raise ValueError('Runtime binding requires a successful capture')
    table=next(v for v in state['runtime_vtables'] if v['object']=='/Script/CoreUObject.Default__Object')
    matches=[]
    for index,value in enumerate(table['entries']):
        if value is None:continue
        rva=int(value,0)
        instructions=list(Cs(CS_ARCH_X86,CS_MODE_64).disasm(code(rva,0x400),base+rva))
        flags=[i for i in instructions if i.address<base+rva+0xA0 and i.mnemonic=='test' and i.op_str=='dword ptr [rdx + 0xb0], 0x400']
        parms=[i for i in instructions if i.mnemonic=='movzx' and 'word ptr [rdi + 0xb6]' in i.op_str]
        parameter_alias=[i for i in instructions[:30] if i.mnemonic=='mov' and i.op_str=='r14, r8']
        if flags and parms and parameter_alias:
            matches.append({'index':index,'rva':value,'evidence':[{'address':hex(i.address),'instruction':i.mnemonic+' '+i.op_str} for i in flags+parms+parameter_alias]})
    if len(matches)!=1:raise ValueError(f'Expected one ProcessEvent candidate; found {len(matches)}')
    match=matches[0]
    return {'module_sha256':state['build']['sha256'],'process_event_vtable_index':match['index'],
            'process_event_rva':match['rva'],'method':'captured root UObject vtable + FunctionFlags native test + ParmsSize reads + third argument alias',
            'image_sha256':hashlib.sha256(data).hexdigest(),'capture':str(capture.resolve()),
            'static_evidence':match['evidence'],'live_function_call_verified':False}


if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--image',type=Path,required=True)
    parser.add_argument('--capture',type=Path,required=True)
    parser.add_argument('--out',type=Path,required=True)
    args=parser.parse_args()
    result=verify(args.image,args.capture)
    args.out.parent.mkdir(parents=True,exist_ok=True)
    args.out.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(result,indent=2))
