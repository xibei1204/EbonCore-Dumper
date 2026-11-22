import contextlib
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from tools.ebon.character_capture import audit_character_captures
from tools.ebon.sdk_codegen import CodegenError
from tools.ebon_cli import main


HERO = '/Game/Blueprints/Character/Player/Player_Test.Player_Test_C'
WEAPON = '/Game/Weapons/Weapon_Test.Weapon_Test_C'


def fixture(root, name='capture', *, loaded=True, body=True):
    capture = root / name
    (capture / 'data').mkdir(parents=True)
    (capture / 'reflection').mkdir()
    def write(path, rows):
        (capture / path).write_text(''.join(json.dumps(row) + '\n' for row in rows), encoding='utf-8')
    write('resource_state.json', [dict(status='complete', success=True, build_sha256='synthetic')])
    table = '/Game/DataTable/Character/DT_Character.DT_Character'
    write('data/table.jsonl', [dict(kind='DataTable', object=table),
        dict(row='1090', values=dict(ElementData=dict(_type='/Script/HTGame.CharacterItemData',
             value=dict(CharacterActorClass=dict(asset_path=HERO)))))])
    entries = [dict(object=table, file='data/table.jsonl', kind='DataTable')]
    write('reflection/types.jsonl', [dict(kind='class', path=HERO)] if loaded else [])
    if body:
        path = '/Game/Maps/Test.Test.PersistentLevel.Pawn_123'
        entries.append(dict(object=path, file='data/player.jsonl', **{'class': 'Player_Test_C'}))
        write('data/player.jsonl', [dict(object=path, **{'class': HERO}, kind='RuntimeActor', is_default=False,
            values=dict(_super=dict(GrantedAbilities=[dict(AbilityClass=dict(object='/Game/Abilities/Test.Test_C'),
                InputID=dict(value=2))], PassiveAbilities=[], DefaultWeaponList=[dict(asset_path=WEAPON)])))])
    write('resource_manifest.jsonl', entries)
    return capture


class CharacterCaptureTests(unittest.TestCase):
    def test_inventory_row_and_mesh_do_not_establish_a_loaded_player(self):
        with tempfile.TemporaryDirectory() as folder:
            capture = fixture(Path(folder), loaded=False, body=False)
            before = {p: p.read_bytes() for p in capture.rglob('*') if p.is_file()}
            result = audit_character_captures([capture], '1090', include_network=False)
            row, = result['captures']
            self.assertFalse(result['success'])
            self.assertEqual(row['character_class'], HERO)
            self.assertFalse(row['class_loaded'])
            self.assertIn('generated_class_not_loaded', row['missing'])
            self.assertIn('character_body_not_observed', row['missing'])
            self.assertEqual(before, {p: p.read_bytes() for p in before})

    def test_runtime_body_is_selected_by_class_and_inherited_lists_are_preserved(self):
        with tempfile.TemporaryDirectory() as folder:
            capture = fixture(Path(folder))
            result = audit_character_captures([capture], 'Player_Test', include_network=False)
            row, = result['captures']
            self.assertTrue(row['configuration_observed'])
            self.assertEqual(row['configurations'][0]['abilities'][0]['input_id'], 2)
            self.assertEqual(row['configurations'][0]['weapon_classes'], [WEAPON])
            self.assertFalse(result['success'])
            self.assertIn('network_not_checked', row['missing'])
            body = capture / 'data/player.jsonl'
            data = json.loads(body.read_text())
            data['values']['_super']['GrantedAbilities'][0]['InputID'] = {'_unresolved': 'no numeric value'}
            body.write_text(json.dumps(data))
            row, = audit_character_captures([capture], '1090', include_network=False)['captures']
            self.assertFalse(row['configuration_observed'])
            self.assertIn('GrantedAbilities[0]_unresolved', row['configurations'][0]['missing'])

    def test_exact_character_and_weapon_layouts_must_coexist_with_configuration(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            first = fixture(root)
            second = fixture(root, 'second', body=False)
            for capture in (first, second):
                (capture / 'reflection/network_metadata.json').write_text('{}')
            base = dict(owner_class='/Script/HTGame.HTPlayerCharacter', status='complete')
            hero = dict(owner_class=HERO, status='complete')
            weapon = dict(owner_class=WEAPON, status='complete')
            def network(layouts):
                return dict(module_sha256='synthetic', layouts=layouts)
            with patch('tools.ebon.character_capture.build_coverage_report', return_value=network([base])):
                self.assertFalse(audit_character_captures([first], '1090')['success'])
            with patch('tools.ebon.character_capture.build_coverage_report', return_value=network([hero])):
                self.assertIn('exact_weapon_layout_not_observed', audit_character_captures([first], '1090')['captures'][0]['missing'])
            with patch('tools.ebon.character_capture.build_coverage_report', side_effect=[network([]), network([hero, weapon])]):
                self.assertFalse(audit_character_captures([first, second], '1090')['success'])
            with patch('tools.ebon.character_capture.build_coverage_report', return_value=network([hero, weapon])):
                result = audit_character_captures([first], '1090')
                self.assertTrue(result['success'])
                self.assertFalse(result['client_verified'])

    def test_unsafe_body_paths_and_conflicting_builds_are_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            first, second = fixture(root), fixture(root, 'second')
            (second / 'resource_state.json').write_text(json.dumps(dict(status='complete', build_sha256='other')))
            with self.assertRaisesRegex(CodegenError, 'single recorded build'):
                audit_character_captures([first, second], '1090', include_network=False)
            reflected_state = first / 'reflection/dump_state.json'
            reflected_state.write_text(json.dumps(dict(build=dict(sha256='different'))))
            with self.assertRaisesRegex(CodegenError, 'reflection evidence build'):
                audit_character_captures([first], '1090', include_network=False)
            reflected_state.unlink()
            manifest = first / 'resource_manifest.jsonl'
            entries = [json.loads(line) for line in manifest.read_text().splitlines()]
            entries[1]['file'] = '../escaped.jsonl'
            manifest.write_text(''.join(json.dumps(row) + '\n' for row in entries))
            with self.assertRaisesRegex(CodegenError, 'escapes'):
                audit_character_captures([first], '1090', include_network=False)

    def test_cli_reports_missing_data_and_protects_sources(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            capture = fixture(root, loaded=False, body=False)
            output = root / 'report.json'
            args = ['audit-character', '--capture', str(capture), '--character', '1090', '--skip-network']
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(main(args + ['--out', str(output)]), 1)
                self.assertEqual(main(args + ['--out', str(output)]), 2)
                self.assertEqual(main(args + ['--out', str(capture / 'bad.json')]), 2)
            self.assertEqual(json.loads(output.read_text())['format'], 'EbonCharacterCaptureAudit-v1')
            self.assertFalse((capture / 'bad.json').exists())
