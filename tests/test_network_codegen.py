import json
from pathlib import Path
import tempfile
import unittest
import struct

from tests.test_cpp_sdk import cpp_fixture
from tools.ebon.cpp_sdk import generate_cpp_sdk
from tools.ebon.sdk_codegen import CodegenError


class NetworkCodegenTests(unittest.TestCase):
    def fixture(self, root):
        dump=cpp_fixture(root)
        rows=[json.loads(line) for line in (dump/'types.jsonl').read_text().splitlines()]
        rpc=next(t for t in rows if t['name']=='Adjust')
        rpc.update(flags='0x2000c0',rpc_id=0,rpc_response_id=19)
        notify=next(t for t in rows if t['name']=='GetPosition')
        notify.update(path='/Script/Test.Actor.OnRep_Health',name='OnRep_Health')
        actor=next(t for t in rows if t['name']=='Actor')
        actor['fields'][0].update(flags='0x100000020',rep_index=0,rep_condition=3,rep_notify='OnRep_Health')
        (dump/'types.jsonl').write_text(''.join(json.dumps(t)+'\n' for t in rows))
        return dump

    def test_exports_zero_ids_direction_reliability_and_rep_notify(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);dump=self.fixture(root);output=root/'sdk'
            manifest=generate_cpp_sdk(dump,output)
            data=json.loads((output/'network_metadata.json').read_text())
            self.assertEqual(data['rpcs'][0]['rpc_id'],0)
            self.assertEqual(data['rpcs'][0]['direction'],'server')
            self.assertTrue(data['rpcs'][0]['reliable'])
            self.assertIsNone(data['rpcs'][0]['field_net_index'])
            self.assertEqual(data['properties'][0]['rep_index'],0)
            self.assertEqual(data['properties'][0]['rep_notify'],'OnRep_Health')
            self.assertIsNone(data['properties'][0]['rep_layout_handle'])
            self.assertIsNone(data['properties'][0]['lifetime_condition'])
            self.assertEqual(manifest['network']['rep_indices_not_sentinel'],1)
            self.assertIn('HasObservedWireMapping = false',(output/'Network.hpp').read_text())

    def test_legacy_missing_indices_remain_unknown(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);dump=self.fixture(root)
            rows=[json.loads(line) for line in (dump/'types.jsonl').read_text().splitlines()]
            for t in rows:
                t.pop('rpc_id',None);t.pop('rpc_response_id',None)
                for field in t['fields']:field.pop('rep_index',None)
            (dump/'types.jsonl').write_text(''.join(json.dumps(t)+'\n' for t in rows))
            output=root/'sdk';generate_cpp_sdk(dump,output)
            data=json.loads((output/'network_metadata.json').read_text())
            self.assertIsNone(data['rpcs'][0]['rpc_id'])
            self.assertIsNone(data['properties'][0]['rep_index'])
            self.assertEqual(data['summary']['rpc_ids_read'],0)

    def test_class_records_are_cross_checked_and_not_relabelled_as_handles(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);dump=self.fixture(root)
            raw=dict(format='EbonNetwork-v1',rpcs=[dict(path='/Script/Test.Actor.Adjust',rpc_id=0,rpc_response_id=19)],
                     properties=[dict(owner='/Script/Test.Actor',property='Health',rep_index=0)],
                     class_tables=[dict(kind='class_reps',class_='unused',header_offset=248,entries=[dict(owner='/Script/Test.Actor',property='Health',rep_index=0,array_index=0)])])
            raw['class_tables'][0]['class']='/Script/Test.Actor'
            (dump/'network_metadata.json').write_text(json.dumps(raw))
            output=root/'sdk';manifest=generate_cpp_sdk(dump,output)
            self.assertEqual(manifest['network']['rep_indices_validated'],1)
            text=(output/'Network.hpp').read_text()
            self.assertIn('std::array<FClassRepRecord, 1>',text)
            self.assertIn('HasObservedWireMapping = false',text)

    def test_refuses_guessed_wire_mapping_and_mismatched_counts(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);dump=self.fixture(root)
            raw=dict(format='EbonNetwork-v1',rpcs=[],properties=[])
            (dump/'network_metadata.json').write_text(json.dumps(raw))
            with self.assertRaisesRegex(CodegenError,'row counts'):generate_cpp_sdk(dump,root/'sdk')
            raw.update(rpcs=[dict(path='/Script/Test.Actor.Adjust',rpc_id=0,rpc_response_id=19,field_net_index=5)],
                       properties=[dict(owner='/Script/Test.Actor',property='Health',rep_index=0)])
            (dump/'network_metadata.json').write_text(json.dumps(raw))
            with self.assertRaisesRegex(CodegenError,'FClassNetCache'):generate_cpp_sdk(dump,root/'sdk2')

    def test_runtime_snapshot_does_not_replace_verified_layout_source(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);original=self.fixture(root)
            runtime_root=root/'runtime';runtime_root.mkdir();runtime=self.fixture(runtime_root)
            state=json.loads((runtime/'dump_state.json').read_text())
            state.update(success=False,status='failed',errors=['dynamic blueprint changed'])
            (runtime/'dump_state.json').write_text(json.dumps(state))
            output=root/'sdk';manifest=generate_cpp_sdk(original,output,network_capture=runtime)
            self.assertEqual(manifest['source_objects'],1)
            self.assertFalse(manifest['network_source']['reflection_capture_success'])
            self.assertEqual(manifest['network_source']['reflection_errors'],1)
            state['build']['sha256']='wrong';(runtime/'dump_state.json').write_text(json.dumps(state))
            with self.assertRaisesRegex(CodegenError,'SHA-256'):generate_cpp_sdk(original,root/'sdk2',network_capture=runtime)

    def test_unloading_blueprint_flags_are_not_exported_as_rpc_ids(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);dump=self.fixture(root)
            rows=[json.loads(l) for l in (dump/'types.jsonl').read_text().splitlines()]
            rpc=next(t for t in rows if t['name']=='Adjust')
            rpc.update(complete=False,flags='0x45341c6b',rpc_id=16113)
            (dump/'types.jsonl').write_text(''.join(json.dumps(t)+'\n' for t in rows))
            output=root/'sdk';manifest=generate_cpp_sdk(dump,output)
            data=json.loads((output/'network_metadata.json').read_text())
            self.assertEqual(data['rpcs'],[])
            self.assertEqual(manifest['network']['excluded_unstable_records'],1)


    def test_multi_scene_indices_are_replayed_and_conflicts_remain_unavailable(self):
        from tests.test_runtime_mapping import runtime_fixture
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);primary=runtime_fixture(root)
            other_root=root/'other';other_root.mkdir();other=runtime_fixture(other_root)
            for source in (primary,other):
                rows=[json.loads(line) for line in (source/'types.jsonl').read_text().splitlines()]
                fn=next(t for t in rows if t['name']=='GetPosition');fn['flags']='0x2000c0'
                (source/'types.jsonl').write_text(''.join(json.dumps(t)+'\n' for t in rows))
                metadata=json.loads((source/'network_metadata.json').read_text())
                metadata['rpcs'].append(dict(path=fn['path'],rpc_id=None,rpc_response_id=None))
                (source/'network_metadata.json').write_text(json.dumps(metadata))
            mapping=json.loads((other/'runtime_mapping.json').read_text())
            memory=bytearray((other/'network_memory.bin').read_bytes())
            struct.pack_into('<ii',memory,0x1028,2,2)
            struct.pack_into('<QII',memory,0x2000,int(fn['address'],0),0,456)
            adjust=next(t for t in rows if t['name']=='Adjust')
            struct.pack_into('<QII',memory,0x2018,int(adjust['address'],0),1,123)
            cache=mapping['class_net_caches'][0]
            cache['header_bytes']=memory[0x1000:0x1030].hex()
            cache['entries']=[dict(path=fn['path'],field_net_index=0,checksum=456,record_bytes=memory[0x2000:0x2018].hex()),
                              dict(path=adjust['path'],field_net_index=1,checksum=123,record_bytes=memory[0x2018:0x2030].hex())]
            (other/'network_memory.bin').write_bytes(memory);(other/'runtime_mapping.json').write_text(json.dumps(mapping))
            sdk=root/'sdk';manifest=generate_cpp_sdk(primary,sdk,extra_network_captures=[other])
            data=json.loads((sdk/'network_metadata.json').read_text())
            rpcs={r['path']:r for r in data['rpcs']}
            self.assertIsNone(rpcs[adjust['path']]['field_net_index'])
            self.assertEqual(rpcs[fn['path']]['field_net_index'],0)
            self.assertEqual(manifest['network']['field_index_conflicts'],1)
            self.assertFalse(manifest['network']['packet_mapping_verified'])
            memory[0x2018]^=1;(other/'network_memory.bin').write_bytes(memory)
            with self.assertRaisesRegex(CodegenError,'bytes differ'):generate_cpp_sdk(primary,root/'bad',extra_network_captures=[other])

    def test_failed_scene_rejects_bad_table_but_replays_valid_cache(self):
        from tests.test_runtime_mapping import runtime_fixture
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);primary=runtime_fixture(root)
            other_root=root/'other';other_root.mkdir();other=runtime_fixture(other_root)
            state=json.loads((other/'dump_state.json').read_text());state.update(success=False,status='failed',errors=['unstable blueprint'])
            (other/'dump_state.json').write_text(json.dumps(state))
            data=json.loads((other/'network_metadata.json').read_text())
            data['class_tables']=[{'kind':'class_net_fields','class':'Transient.Unloaded',
                                  'entries':[{'position':0,'path':'Transient.Unloaded.Bad'}]}]
            (other/'network_metadata.json').write_text(json.dumps(data))
            sdk=root/'sdk';generate_cpp_sdk(primary,sdk,extra_network_captures=[other])
            result=json.loads((sdk/'network_metadata.json').read_text())
            self.assertEqual(result['additional_captures'][0]['summary']['rejected_class_tables'],1)
            self.assertEqual(result['summary']['rpc_field_net_indices_observed'],1)


if __name__=='__main__':unittest.main()
