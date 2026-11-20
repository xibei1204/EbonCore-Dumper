"""Replay bounded in-process memory captures and cross-check Unreal network tables."""
from bisect import bisect_right
import json
from pathlib import Path
import struct

from .network_fields import network_fields


class CapturedMemory:
    def __init__(self, capture):
        capture=Path(capture)
        index=json.loads((capture/'network_memory.json').read_text(encoding='utf-8'))
        self.data=(capture/'network_memory.bin').read_bytes()
        if index.get('format')!='EbonNetworkMemory-v1' or len(self.data)>64*1024*1024:
            raise ValueError('Unsupported or excessive network memory capture')
        self.blocks=[];self.pages={}
        for record in index['blocks']:
            address=int(record['address'],0);offset=record['offset'];size=record['size']
            if offset<0 or size<0 or offset+size>len(self.data):raise ValueError('Memory capture bounds are invalid')
            entry=(address,offset,size);self.blocks.append(entry)
            for page in range(address//4096,(address+size-1)//4096+1):self.pages.setdefault(page,[]).append(entry)

    def read(self,address,size):
        result=bytearray()
        while size:
            matches=[r for r in self.pages.get(address//4096,[]) if r[0]<=address<r[0]+r[2]]
            if not matches:raise KeyError(hex(address))
            start,offset,length=max(matches,key=lambda r:r[0]+r[2])
            n=min(size,start+length-address)
            result.extend(self.data[offset+address-start:offset+address-start+n]);size-=n;address+=n
        return bytes(result)

    def number(self,address,fmt):return struct.unpack(fmt,self.read(address,struct.calcsize(fmt)))[0]
    def u64(self,a):return self.number(a,'<Q')
    def i32(self,a):return self.number(a,'<i')
    def u32(self,a):return self.number(a,'<I')
    def u16(self,a):return self.number(a,'<H')
    def u8(self,a):return self.number(a,'<B')
    def array(self,a):
        data=self.u64(a);n=self.i32(a+8);capacity=self.i32(a+12)
        if data<65536 or data&7 or not 1<=n<=8192 or not n<=capacity<=65536:raise ValueError('Invalid array')
        return data,n


class RuntimeReplay:
    def __init__(self,capture):
        self.capture=Path(capture);self.memory=CapturedMemory(capture)
        self.state=json.loads((self.capture/'dump_state.json').read_text(encoding='utf-8'))
        with (self.capture/'types.jsonl').open(encoding='utf-8') as stream:owners=[json.loads(l) for l in stream]
        with (self.capture/'objects.jsonl').open(encoding='utf-8') as stream:objects=[json.loads(l) for l in stream]
        self.class_indices={o['index']:o for o in objects if o['class']=='Class' or 'GeneratedClass' in o['class']}
        self.classes={t['path']:t for t in owners if t['kind']=='class' and t.get('complete') is not False}
        self.class_addresses={int(t['address'],0):t for t in self.classes.values()}
        self.fields,self.by_path=network_fields(owners)

    def belongs(self,owner,cls):
        seen=set()
        while cls and cls not in seen:
            if owner==cls:return True
            seen.add(cls);cls=self.classes.get(cls,{}).get('super')
        return False

    def cache(self,address):
        m=self.memory
        try:
            header=m.read(address,48);base=m.i32(address);super_=m.u64(address+8)
            index=m.i32(address+16);serial=m.i32(address+20)
            cls=self.class_indices[index]['path']
            if cls not in self.classes or not 0<serial<0x40000000 or not 0<=base<1000000:return None
            data,n,capacity=struct.unpack_from('<Qii',header,32)
            if not 0<=n<=8192 or not n<=capacity<=65536 or (n and (data<65536 or data&7)) or base+n>1000000:return None
            parent=None
            if super_:
                parent=self.class_indices[m.i32(super_+16)]['path']
                if self.classes[cls].get('super')!=parent or m.i32(super_)+m.i32(super_+40)!=base:return None
            elif base!=0 or (not n and self.classes[cls].get('super')):return None
            for stride in (24,16):
                rows=[]
                try:
                    for i in range(n):
                        at=data+i*stride;raw=m.read(at,stride);f=self.fields[m.u64(at)&~1]
                        if f['owner']!=cls or m.i32(at+8)!=base+i or (stride==24 and m.u8(at+16)>1):break
                        rows.append(dict(path=f['path'],field_net_index=base+i,checksum=m.u32(at+12),
                                         incompatible=bool(m.u8(at+16)) if stride==24 else False,
                                         record_address=hex(at),record_bytes=raw.hex()))
                    if len(rows)==n:return dict(kind='class_net_cache',class_=cls,address=hex(address),header_bytes=header.hex(),
                                               weak_class_index=index,weak_class_serial=serial,super_address=hex(super_),fields_base=base,
                                               super_class=parent,field_count=n,field_index_start=base,field_index_end_exclusive=base+n,
                                               field_index_end_inclusive=base+n-1 if n else None,
                                               field_stride=stride if n else 24,class_checksum=m.u32(address+24),entries=rows,validation='runtime_cross_checked')
                except (KeyError,ValueError,struct.error):continue
        except (KeyError,ValueError,struct.error):pass
        return None

    def layout_at_arrays(self,header,cls):
        m=self.memory
        try:
            data,pcount=m.array(header);cmds,ccount=m.array(header+16)
            if pcount>2048 or ccount>8192:return None
            first=self.fields[m.u64(data)&~1]
            if first['rpc'] or not self.belongs(first['owner'],cls):return None
            for ps,range_offset in ((48,28),(48,24),(56,28),(56,24),(64,28),(72,28)):
                try:
                    parents=[];partial=False
                    for p in range(pcount):
                        at=data+p*ps
                        try:raw=m.read(at,ps)
                        except KeyError:
                            partial=True;break
                        f=self.fields[m.u64(at)&~1]
                        start=m.u16(at+range_offset);end=m.u16(at+range_offset+2);array=m.i32(at+16);condition=m.u8(at+32)
                        if f['rpc'] or not self.belongs(f['owner'],cls) or not 0<=array<f.get('array_dim',1) or not 0<=start<=end<=ccount or condition>32:break
                        parents.append(dict(path=f['path'],parent_index=p,array_index=array,cmd_start=start,cmd_end=end,
                                            lifetime_condition=condition,record_bytes=raw.hex()))
                    if len(parents)!=pcount and (not partial or len(parents)<3):continue
                    for cs in (32,40):
                        try:
                            commands=[];scopes=[];skipped=[]
                            for c in range(ccount):
                                at=cmds+c*cs;raw=m.read(at,cs);kind=m.u8(at+28)
                                while scopes and c>=scopes[-1][0]:scopes.pop()
                                if kind==1:
                                    commands.append(dict(cmd_index=c,type=1,return_=True,record_bytes=raw.hex()));continue
                                f=self.fields[m.u64(at)&~1];parent=m.u16(at+22);handle=m.u16(at+20);offset=m.i32(at+12);end=m.u16(at+8)
                                if f['rpc'] or not 0<=parent<pcount or not 1<=handle<65536 or kind>48 or not 0<=offset<0x10000000 or (kind==0 and not c<end<=ccount):break
                                if parent>=len(parents):skipped.append(c);continue
                                if not parents[parent]['cmd_start']<=c<parents[parent]['cmd_end']:break
                                top=self.by_path[parents[parent]['path']]
                                if top['address']==f['address'] and offset!=f['offset']+parents[parent]['array_index']*f['element_size']:break
                                commands.append(dict(path=f['path'],parent_path=parents[parent]['path'],cmd_index=c,
                                                     relative_handle=handle,parent_index=parent,offset=offset,type=kind,end_cmd=end,
                                                     scope_cmd_index=scopes[-1][1] if scopes else -1,checksum=m.u32(at+24),
                                                     record_address=hex(at),record_bytes=raw.hex()))
                                if kind==0:scopes.append((end,c))
                            if len(commands)+len(skipped)==ccount and any('relative_handle' in row for row in commands):
                                return dict(kind='rep_layout',class_=cls,address=hex(header),anchor='parent_array_header',
                                            array_header_bytes=m.read(header,32).hex(),parent_stride=ps,parent_ranges_offset=range_offset,command_stride=cs,
                                            parents=parents,commands=commands,validation='runtime_cross_checked',partial=partial,
                                            declared_parent_count=pcount,declared_command_count=ccount,skipped_cmd_indices=skipped)
                        except (KeyError,ValueError,struct.error):continue
                except (KeyError,ValueError,struct.error):continue
        except (KeyError,ValueError,struct.error):pass
        return None

    def scan(self):
        caches={};layouts={};m=self.memory;array_candidates=[];owner_positions=[]
        for address,offset,size in m.blocks:
            block=m.data[offset:offset+size]
            # Deduplicated ranges can start mid-pointer. Align by virtual
            # address and join adjacent saved blocks for boundary records.
            for pos in range((-address)&7,size,8):
                try:
                    if pos+24<=size:index,serial=struct.unpack_from('<ii',block,pos+16)
                    else:index=m.i32(address+pos+16);serial=m.i32(address+pos+20)
                    if index in self.class_indices and 0<serial<0x40000000:
                        cache=self.cache(address+pos)
                        if cache and cache['entries']:
                            cache['class']=cache.pop('class_');caches[address+pos]=cache
                except KeyError:pass
                try:pointer=struct.unpack_from('<Q',block,pos)[0] if pos+8<=size else m.u64(address+pos)
                except KeyError:continue
                if pointer in self.class_addresses:owner_positions.append((address+pos,self.class_addresses[pointer]['path']))
                if pointer<65536 or pointer&7:continue
                if pos+32<=size:
                    n,capacity,cmds,nc,cc=struct.unpack_from('<iiQii',block,pos+8)
                    if not 1<=n<=8192 or not n<=capacity<=65536 or cmds<65536 or cmds&7 or not 1<=nc<=8192 or not nc<=cc<=65536:continue
                try:
                    ptr,n=m.array(address+pos);cmds,nc=m.array(address+pos+16)
                    if m.u64(ptr)&~1 in self.fields:array_candidates.append(address+pos)
                except (KeyError,ValueError,struct.error):pass
        # Empty cache headers are meaningful only when reached from an
        # observed child, not as unrelated zero-filled memory patterns.
        for child in list(caches.values()):
            parent=int(child['super_address'],0);seen=set()
            while parent and parent not in seen and len(seen)<64:
                seen.add(parent)
                if parent in caches:break
                cache=self.cache(parent)
                if not cache:break
                cache['class']=cache.pop('class_');caches[parent]=cache
                parent=int(cache['super_address'],0)
        owner_positions.sort();positions=[p[0] for p in owner_positions]
        for header in set(array_candidates):
            begin=bisect_right(positions,header-256);end=bisect_right(positions,header+512)
            for owner_position,cls in owner_positions[begin:end]:
                if owner_position-header not in (56,64,72,80,96,112):continue
                layout=self.layout_at_arrays(header,cls)
                if layout:
                    layout['class']=layout.pop('class_');layout['owner_pointer_address']=hex(owner_position)
                    layouts[header]=layout;break
        return dict(format='EbonRuntimeMapping-v1',module_sha256=self.state['build']['sha256'],
                    capture=str(self.capture.resolve()),class_net_caches=list(caches.values()),rep_layouts=list(layouts.values()),
                    packet_verified=False)


if __name__=='__main__':
    import argparse
    parser=argparse.ArgumentParser();parser.add_argument('--capture',type=Path,required=True);args=parser.parse_args()
    result=RuntimeReplay(args.capture).scan()
    path=args.capture/'runtime_mapping.json';path.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(dict(caches=len(result['class_net_caches']),layouts=len(result['rep_layouts']),output=str(path)),indent=2))
