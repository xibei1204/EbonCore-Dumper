from copy import deepcopy
import json
from pathlib import Path
import tempfile
import unittest

from tests.test_cpp_sdk import cpp_fixture
from tools.ebon.cpp_sdk import Generator, generate_cpp_sdk
from tools.ebon.sdk_codegen import CodegenError, _validate_dump
from tools.ebon.sdk_merge import merge_sdk_captures


class SdkMergeTests(unittest.TestCase):
    def test_recovers_observed_types_and_retains_layout_conflicts(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);first=cpp_fixture(root)
            second_root=root/'second';second_root.mkdir();second=cpp_fixture(second_root)
            rows=[json.loads(line) for line in (second/'types.jsonl').read_text().splitlines()]
            missing=rows.pop(5)['path']
            rows[1]['fields'][0]['name']='ChangedX'
            (second/'types.jsonl').write_text(''.join(json.dumps(r)+'\n' for r in rows))
            state=json.loads((second/'dump_state.json').read_text());state.update(types=len(rows),excluded_types=[missing+': unloaded'])
            (second/'dump_state.json').write_text(json.dumps(state))
            output=root/'merged';report=merge_sdk_captures([first,second],output,['login','gameplay'])
            self.assertEqual(report['summary']['types_added_to_primary'],1)
            self.assertIn(missing,report['excluded_types_recovered'])
            self.assertEqual(report['summary']['layout_conflicts'],1)
            self.assertEqual(report['pending_scenes'],['updater','character_creation'])
            self.assertEqual(_validate_dump(output)[0]['types'],8)
            self.assertEqual(json.loads((output/'merge_report.json').read_text())['summary'],report['summary'])

    def test_build_mismatch_does_not_create_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);first=cpp_fixture(root);second_root=root/'second';second_root.mkdir();second=cpp_fixture(second_root)
            state=json.loads((second/'dump_state.json').read_text());state['build']['sha256']='other'
            (second/'dump_state.json').write_text(json.dumps(state))
            with self.assertRaisesRegex(CodegenError,'SHA-256'):merge_sdk_captures([first,second],root/'merged')
            self.assertFalse((root/'merged').exists())

    def test_parameter_symbols_survive_reordering_and_new_collisions(self):
        with tempfile.TemporaryDirectory() as temporary:
            state,owners,_=_validate_dump(cpp_fixture(Path(temporary)))
            original=Generator(state,owners).names
            fn=deepcopy(owners[4]);fn['path']='/Script/Test.Other.Adjust'
            after=Generator(state,list(reversed(owners))+[fn]).names
            for owner in owners:
                if owner['kind']=='function':self.assertEqual(original[owner['path']],after[owner['path']])

    def test_replays_packed_alignment_and_rejects_byte_disagreement(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);source=cpp_fixture(root)
            rows=[json.loads(line) for line in (source/'types.jsonl').read_text().splitlines()]
            rows[1]['address']='0x12340000'
            (source/'types.jsonl').write_text(''.join(json.dumps(r)+'\n' for r in rows))
            raw=bytearray(96);raw[88:92]=(12).to_bytes(4,'little');raw[92:96]=(0x10004).to_bytes(4,'little')
            evidence={'format':'EbonReflectionEvidence-v1','module_sha256':'fixture','size_offset':88,'alignment_offset':92,'alignment_width':2,
                      'types':[{'path':rows[1]['path'],'address':rows[1]['address'],'bytes':raw.hex()}]}
            (source/'reflection_evidence.json').write_text(json.dumps(evidence))
            manifest=generate_cpp_sdk(source,root/'sdk')
            self.assertEqual(manifest['alignment_byte_verified_layouts'],1)
            self.assertEqual(manifest['alignment_abi_verified_layouts'],0)
            raw[88:92]=(16).to_bytes(4,'little');evidence['types'][0]['bytes']=raw.hex()
            (source/'reflection_evidence.json').write_text(json.dumps(evidence))
            with self.assertRaisesRegex(CodegenError,'size differs'):generate_cpp_sdk(source,root/'bad')

    def test_symbol_baseline_emits_reviewable_compatibility_migration(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);source=cpp_fixture(root);baseline=root/'previous.json'
            baseline.write_text(json.dumps({'/Script/Test.Actor.Adjust':'Params_OldAdjust'}))
            generate_cpp_sdk(source,root/'sdk',symbol_baseline=baseline)
            migrations=json.loads((root/'sdk/symbol_migration.json').read_text())
            self.assertTrue(migrations[0]['compatibility_alias'])
            self.assertIn('using Params_OldAdjust = ',(root/'sdk/SymbolAliases.hpp').read_text())

    def test_recovers_property_from_own_slot_and_rejects_changed_header(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);source=cpp_fixture(root)
            rows=[json.loads(line) for line in (source/'types.jsonl').read_text().splitlines()]
            rows[1]['address']='0x11110000'
            field=rows[2]['fields'][-1];field.update(address='0x22220000',target=None);field['detail']['target']=None
            (source/'types.jsonl').write_text(''.join(json.dumps(r)+'\n' for r in rows))
            raw=bytearray(128)
            for offset,value in [(56,1),(60,12),(76,48)]:raw[offset:offset+4]=value.to_bytes(4,'little')
            raw[120:128]=int(rows[1]['address'],0).to_bytes(8,'little')
            evidence={'format':'EbonReflectionEvidence-v1','module_sha256':'fixture','size_offset':88,'alignment_offset':92,'alignment_width':2,
                      'types':[],'properties':[{'owner':rows[2]['path'],'name':field['name'],'address':field['address'],'bytes':raw.hex()}]}
            (source/'reflection_evidence.json').write_text(json.dumps(evidence))
            result=generate_cpp_sdk(source,root/'sdk');self.assertEqual(result['unresolved_fields'],0)
            recovered=json.loads((root/'sdk/semantic_recovery.json').read_text())
            self.assertEqual(recovered[0]['target'],rows[1]['path'])
            raw[76:80]=(49).to_bytes(4,'little');evidence['properties'][0]['bytes']=raw.hex()
            (source/'reflection_evidence.json').write_text(json.dumps(evidence))
            with self.assertRaisesRegex(CodegenError,'differs from parsed'):generate_cpp_sdk(source,root/'bad')


if __name__=='__main__':unittest.main()
