import json
from pathlib import Path
import struct
import tempfile
import unittest

from tests.test_cpp_sdk import cpp_fixture
from tools.ebon.cpp_sdk import generate_cpp_sdk
from tools.ebon.sdk_codegen import CodegenError
from tools.ebon.network_fields import network_fields
from tools.ebon.runtime_mapping import checked_runtime_mapping
from tools.ebon.runtime_network import RuntimeReplay
from tools.ebon.network_coverage import build_coverage_report


def runtime_fixture(root):
    dump=cpp_fixture(root)
    rows=[json.loads(line) for line in (dump/'types.jsonl').read_text().splitlines()]
    for i,t in enumerate(rows):
        t['address']=hex(0x50000+i*0x100);t['complete']=True
        for j,f in enumerate(t['fields']):f['address']=hex(0x80000+i*0x1000+j*0x100)
    cls=next(t for t in rows if t['name']=='Actor');fn=next(t for t in rows if t['name']=='Adjust')
    fn['flags']='0x2000c0';health=cls['fields'][0];health.update(flags='0x20',rep_index=0)
    (dump/'types.jsonl').write_text(''.join(json.dumps(t)+'\n' for t in rows))
    objects=[dict(index=5,address=cls['address'],path=cls['path'],name='Actor',**{'class':'Class'}),
             dict(index=6,address='0x70000',path='Transient.Driver',name='Driver',**{'class':'IpNetDriver'})]
    (dump/'objects.jsonl').write_text(''.join(json.dumps(o)+'\n' for o in objects))
    state=json.loads((dump/'dump_state.json').read_text());state['objects']=2;(dump/'dump_state.json').write_text(json.dumps(state))
    image=bytearray(0x10000)
    def put(address,fmt,*values):struct.pack_into(fmt,image,address-0x70000,*values)
    put(0x70000,'<Q',0x71000)
    put(0x71000,'<i',0);put(0x71010,'<ii',5,1);put(0x71020,'<Qii',0x72000,1,1)
    put(0x72000,'<QII',int(fn['address'],0),0,123)
    put(0x73100,'<Qii',0x74000,1,1);put(0x73110,'<Qii',0x75000,2,2)
    put(0x73138,'<Q',int(cls['address'],0))
    put(0x74000,'<Q',int(health['address'],0));put(0x7401c,'<HH',0,1)
    put(0x75000,'<Q',int(health['address'],0));put(0x7500c,'<i',40);put(0x75014,'<HHI',7,0,456);put(0x7501c,'<B',5)
    put(0x7503c,'<B',1)
    def raw(address,size):return image[address-0x70000:address-0x70000+size].hex()
    (dump/'network_memory.bin').write_bytes(image)
    (dump/'network_memory.json').write_text(json.dumps(dict(format='EbonNetworkMemory-v1',blocks=[dict(address='0x70000',offset=0,size=len(image))])))
    mapping=dict(format='EbonRuntimeMapping-v1',module_sha256='fixture',capture=str(dump.resolve()),
                 class_net_caches=[dict(kind='class_net_cache',**{'class':cls['path']},address='0x71000',header_bytes=raw(0x71000,48),field_stride=24,
                                       entries=[dict(path=fn['path'],field_net_index=0,checksum=123,record_bytes=raw(0x72000,24))],validation='runtime_cross_checked')],
                 rep_layouts=[dict(kind='rep_layout',**{'class':cls['path']},address='0x73100',anchor='parent_array_header',
                                   owner_pointer_address='0x73138',array_header_bytes=raw(0x73100,32),parent_stride=48,parent_ranges_offset=28,command_stride=32,
                                   parents=[dict(path=cls['path']+'.Health',parent_index=0,array_index=0,cmd_start=0,cmd_end=1,lifetime_condition=0,record_bytes=raw(0x74000,48))],
                                   commands=[dict(path=cls['path']+'.Health',parent_path=cls['path']+'.Health',cmd_index=0,type=5,relative_handle=7,parent_index=0,offset=40,record_bytes=raw(0x75000,32)),
                                             dict(cmd_index=1,type=1,record_bytes=raw(0x75020,32))],validation='runtime_cross_checked')])
    (dump/'runtime_mapping.json').write_text(json.dumps(mapping))
    network=dict(format='EbonNetwork-v1',rpcs=[dict(path=fn['path'],rpc_id=None,rpc_response_id=None)],
                 properties=[dict(owner=cls['path'],property='Health',rep_index=0)],
                 active_network_objects=[dict(address='0x70000',path='Transient.Driver',**{'class':'IpNetDriver'})])
    (dump/'network_metadata.json').write_text(json.dumps(network))
    return dump


def dynamic_runtime_fixture(root):
    dump=runtime_fixture(root)
    rows=[json.loads(line) for line in (dump/'types.jsonl').read_text().splitlines()]
    cls=next(t for t in rows if t['name']=='Actor');field=cls['fields'][0]
    field.update(type='ArrayProperty',element_size=16,size=16,
                 detail=dict(type='ArrayProperty',size=16,inner=dict(
                     address='0x8f000',type='FloatProperty',offset=0,
                     element_size=4,array_dim=1,size=4,flags='0x0')))
    (dump/'types.jsonl').write_text(''.join(json.dumps(t)+'\n' for t in rows))
    body=bytearray((dump/'network_memory.bin').read_bytes())
    def put(address,fmt,*values):struct.pack_into(fmt,body,address-0x70000,*values)
    put(0x73110,'<Qii',0x75000,4,4);put(0x7401c,'<HH',0,3)
    body[0x5000:0x5080]=bytes(128)
    put(0x75000,'<QH',int(field['address'],0),3)
    put(0x7500c,'<i',40);put(0x75014,'<HHI',7,0,456)
    put(0x75020,'<Q',0x8f000);put(0x75034,'<HHI',1,0,999);put(0x7503c,'<B',5)
    put(0x7505c,'<B',1);put(0x7507c,'<B',1)
    (dump/'network_memory.bin').write_bytes(body)
    (dump/'runtime_mapping.json').unlink()
    return dump,rows


class RuntimeMappingTests(unittest.TestCase):
    def test_coverage_report_uses_inheritance_and_leaves_capture_unchanged(self):
        with tempfile.TemporaryDirectory() as temporary:
            dump,owners=dynamic_runtime_fixture(Path(temporary))
            actor=next(t for t in owners if t['name']=='Actor')
            actor['super']='/Script/Engine.Character'
            owners.append(dict(kind='class',path='/Script/Engine.Character',name='Character',
                               address='0x60000',size=40,super='/Script/CoreUObject.Object',
                               fields=[],complete=True))
            (dump/'types.jsonl').write_text(''.join(json.dumps(t)+'\n' for t in owners))
            before={p.name:p.read_bytes() for p in dump.iterdir() if p.is_file()}
            report=build_coverage_report(dump)
            self.assertEqual(report['summary']['complete_layouts'],1)
            group=report['groups']['Character']
            self.assertEqual((group['loaded_owner_classes'],group['owner_classes_with_layouts']),(3,1))
            self.assertEqual(group['layouts'][0]['owner_class'],'/Script/Test.Actor')
            self.assertEqual((group['layouts'][0]['parents_declared'],group['layouts'][0]['commands_declared']),(1,4))
            after={p.name:p.read_bytes() for p in dump.iterdir() if p.is_file()}
            self.assertEqual(before,after)

    def test_coverage_report_marks_incomplete_parent_table_as_partial(self):
        with tempfile.TemporaryDirectory() as temporary:
            dump,_=dynamic_runtime_fixture(Path(temporary))
            mapping=RuntimeReplay(dump).scan()
            layout=mapping['rep_layouts'][0]
            body=bytearray((dump/'network_memory.bin').read_bytes())
            struct.pack_into('<ii',body,0x3108,2,2)
            (dump/'network_memory.bin').write_bytes(body)
            layout.update(partial=True,array_header_bytes=body[0x3100:0x3120].hex(),
                          declared_parent_count=1)  # The raw header declares two.
            (dump/'runtime_mapping.json').write_text(json.dumps(mapping))
            report=build_coverage_report(dump)
            self.assertEqual(report['summary']['complete_layouts'],0)
            self.assertEqual(report['summary']['partial_layouts'],1)
            table=report['layouts'][0]
            self.assertEqual((table['parents_observed'],table['parents_declared']),(1,2))

    def test_dynamic_array_inner_commands_replay_and_revalidate(self):
        with tempfile.TemporaryDirectory() as temporary:
            dump,owners=dynamic_runtime_fixture(Path(temporary))
            mapping=RuntimeReplay(dump).scan()
            self.assertEqual(len(mapping['rep_layouts']),1)
            layout=mapping['rep_layouts'][0]
            self.assertEqual((len(layout['parents']),len(layout['commands'])),(1,4))
            inner=layout['commands'][1]
            self.assertEqual(inner['path'],'/Script/Test.Actor.Health.inner')
            self.assertEqual((inner['relative_handle'],inner['scope_cmd_index']),(1,0))
            (dump/'runtime_mapping.json').write_text(json.dumps(mapping))
            active=json.loads((dump/'network_metadata.json').read_text())['active_network_objects']
            verified=checked_runtime_mapping(dump,owners,'fixture',active)
            self.assertEqual(len(verified['handles']),2)
            self.assertEqual(verified['handles'][1]['path'],inner['path'])
            # Changing either sidecar identity or reflected child identity must
            # fail even though the containing array property remains valid.
            inner['path']='/Script/Test.Actor.Health'
            (dump/'runtime_mapping.json').write_text(json.dumps(mapping))
            with self.assertRaisesRegex(CodegenError,'field/parent'):
                checked_runtime_mapping(dump,owners,'fixture',active)
            inner['path']='/Script/Test.Actor.Health.inner'
            (dump/'runtime_mapping.json').write_text(json.dumps(mapping))
            actor=next(t for t in owners if t['name']=='Actor')
            actor['fields'][0]['detail']['inner']['address']='0x8f008'
            with self.assertRaisesRegex(CodegenError,'field/parent'):
                checked_runtime_mapping(dump,owners,'fixture',active)

    def test_legacy_inner_metadata_without_address_is_not_invented(self):
        with tempfile.TemporaryDirectory() as temporary:
            dump,owners=dynamic_runtime_fixture(Path(temporary))
            actor=next(t for t in owners if t['name']=='Actor')
            del actor['fields'][0]['detail']['inner']['address']
            (dump/'types.jsonl').write_text(''.join(json.dumps(t)+'\n' for t in owners))
            replay=RuntimeReplay(dump)
            self.assertNotIn('/Script/Test.Actor.Health.inner',replay.by_path)
            self.assertEqual(replay.scan()['rep_layouts'],[])

    def test_container_child_paths_include_map_and_enum_children(self):
        child=dict(address='0x90000',type='MapProperty',size=80,
                   key=dict(address='0x90100',type='EnumProperty',size=1,
                            underlying=dict(address='0x90200',type='ByteProperty',size=1)),
                   value=dict(address='0x90300',type='OptionalProperty',size=8,
                              inner=dict(address='0x90400',type='FloatProperty',size=4)))
        owners=[dict(kind='class',path='Test.Owner',fields=[dict(
            name='Values',address='0x80000',offset=0,element_size=16,
            array_dim=1,detail=dict(inner=child))])]
        fields,paths=network_fields(owners)
        self.assertEqual(paths['Test.Owner.Values.inner.key.underlying']['address'],0x90200)
        self.assertEqual(paths['Test.Owner.Values.inner.value.inner']['address'],0x90400)
        self.assertEqual({row['owner'] for row in fields.values()},{'Test.Owner'})

    def test_small_capture_blocks_keep_owner_pointers_and_do_not_unpack_past_end(self):
        with tempfile.TemporaryDirectory() as temporary:
            dump=runtime_fixture(Path(temporary))
            index=dict(format='EbonNetworkMemory-v1',blocks=[
                dict(address='0x70000',offset=0,size=0x3139),
                dict(address='0x73139',offset=0x3139,size=9),
                dict(address='0x73142',offset=0x3142,size=0x10000-0x3142),
                dict(address='0x90000',offset=0,size=1)])
            (dump/'network_memory.json').write_text(json.dumps(index))
            mapping=RuntimeReplay(dump).scan()
            self.assertEqual(len(mapping['rep_layouts']),1)
            self.assertEqual(mapping['rep_layouts'][0]['owner_pointer_address'],'0x73138')

    def test_runtime_records_change_manifest_and_cpp_exports(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);dump=runtime_fixture(root);out=root/'sdk'
            manifest=generate_cpp_sdk(dump,out);s=manifest['network']
            self.assertEqual(s['runtime_mapping_status'],'observed_partial')
            self.assertEqual(s['rpc_field_net_indices_observed'],1)
            self.assertEqual(s['rep_layout_handles_observed'],1)
            network=json.loads((out/'network_metadata.json').read_text())
            self.assertEqual(network['rpcs'][0]['field_net_index'],0)
            self.assertEqual(network['properties'][0]['rep_index'],0)
            self.assertEqual(network['properties'][0]['rep_layout_handle'],7)
            text=(out/'Network.hpp').read_text();self.assertIn('HasObservedWireMapping = true',text)
            self.assertIn('PacketMappingVerified = false',text)

    def test_changed_metadata_or_memory_cannot_promote_runtime_status(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);dump=runtime_fixture(root)
            path=dump/'runtime_mapping.json';m=json.loads(path.read_text())
            m['class_net_caches'][0]['entries'][0]['field_net_index']=3;path.write_text(json.dumps(m))
            with self.assertRaisesRegex(CodegenError,'cross-check'):generate_cpp_sdk(dump,root/'bad1')
            m['class_net_caches'][0]['entries'][0]['field_net_index']=0
            m['rep_layouts'][0]['commands'][0]['relative_handle']=0;path.write_text(json.dumps(m))
            with self.assertRaisesRegex(CodegenError,'raw bytes|captured bytes'):generate_cpp_sdk(dump,root/'bad2')
            m['rep_layouts'][0]['commands'][0]['relative_handle']=7;path.write_text(json.dumps(m))
            body=bytearray((dump/'network_memory.bin').read_bytes());body[0x500c]=44;(dump/'network_memory.bin').write_bytes(body)
            with self.assertRaisesRegex(CodegenError,'differ from captured memory'):generate_cpp_sdk(dump,root/'bad3')

    def test_nonmatching_build_cannot_reuse_mapping(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);dump=runtime_fixture(root);p=dump/'runtime_mapping.json';m=json.loads(p.read_text())
            m['module_sha256']='other';p.write_text(json.dumps(m))
            with self.assertRaisesRegex(CodegenError,'SHA-256'):generate_cpp_sdk(dump,root/'sdk')

    def test_captured_memory_is_decoded_without_preexisting_sidecar(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);dump=runtime_fixture(root)
            (dump/'runtime_mapping.json').unlink()
            out=root/'sdk';manifest=generate_cpp_sdk(dump,out)
            self.assertEqual(manifest['network']['field_net_indices_observed'],1)
            self.assertEqual(manifest['network']['rep_layout_handles_observed'],1)
            self.assertTrue((dump/'runtime_mapping.json').is_file())


if __name__=='__main__':unittest.main()
