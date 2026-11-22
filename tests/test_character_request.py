import contextlib
import io
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

from tests.test_character_capture import fixture, HERO
from tools.ebon.character_request import select_character, request_character
from tools.ebon.sdk_codegen import CodegenError

ROOT = Path(__file__).resolve().parents[1]
BUILD = Path(os.environ.get('EBON_TEST_BUILD_DIR', ROOT / 'build'))


@patch('tools.ebon.character_request._process_alive', return_value=True)
class CharacterRequestTests(unittest.TestCase):
    def initialized(self, root):
        capture = root / 'initialized'
        capture.mkdir()
        (capture / 'character_loader_ready.json').write_text(json.dumps(dict(
            format='EbonCharacterLoaderReady-v1', enabled=True, process_id=os.getpid(), build_sha256='synthetic')))
        (capture / 'dump_state.json').write_text(json.dumps(dict(build=dict(sha256='synthetic'))))
        return capture

    def test_selector_uses_the_player_class_in_the_actual_table(self, alive):
        with tempfile.TemporaryDirectory() as folder:
            source = fixture(Path(folder))
            for selector in ('1090', 'Player_Test', HERO):
                target = select_character(source, selector)
                self.assertEqual(target['class_path'], HERO)
                self.assertEqual(len(target['table_sha256']), 64)
            with self.assertRaisesRegex(CodegenError, 'one exact'):
                select_character(source, 'not-a-character')

    def test_old_profile_build_mismatch_and_existing_request_are_rejected(self, alive):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source = fixture(root)
            capture = self.initialized(root)
            ready = capture / 'character_loader_ready.json'
            data = json.loads(ready.read_text())
            data['enabled'] = False
            ready.write_text(json.dumps(data))
            with self.assertRaisesRegex(CodegenError, 'profile'):
                request_character(capture, source, '1090')
            data['enabled'] = True
            data['build_sha256'] = 'different'
            ready.write_text(json.dumps(data))
            with self.assertRaisesRegex(CodegenError, 'different builds'):
                request_character(capture, source, '1090')
            data['build_sha256'] = 'synthetic'
            ready.write_text(json.dumps(data))
            alive.return_value = False
            with self.assertRaisesRegex(CodegenError, 'process has exited'):
                request_character(capture, source, '1090')
            self.assertFalse((capture / 'character_capture.request').exists())
            alive.return_value = True
            request = capture / 'character_capture.request'
            request.write_text('preserve this request')
            with self.assertRaisesRegex(CodegenError, 'already pending'):
                request_character(capture, source, '1090')
            self.assertEqual(request.read_text(), 'preserve this request')
            self.assertFalse(list(capture.glob('.character-*.tmp')))

    def test_request_is_published_atomically_then_export_is_audited(self, alive):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source = fixture(root)
            capture = self.initialized(root)
            observed = {}
            def worker():
                request = capture / 'character_capture.request'
                until = time.monotonic() + 3
                while not request.is_file() and time.monotonic() < until:
                    time.sleep(0.01)
                if not request.is_file():
                    return
                values = dict(line.split('=', 1) for line in request.read_text().splitlines())
                observed.update(values)
                job = capture / 'characters' / ('char_' + values['request_id'])
                job.mkdir(parents=True)
                (job / 'character_load_state.json').write_text(json.dumps(dict(status='export_finished')))
            thread = threading.Thread(target=worker)
            thread.start()
            try:
                with contextlib.redirect_stdout(io.StringIO()), patch(
                    'tools.ebon.character_request.audit_character_captures',
                    return_value=dict(success=False, captures=[dict(missing=['exact_character_layout_not_observed'])])) as audit:
                    result = request_character(capture, source, '1090', timeout=3)
                self.assertFalse(result['success'])
                self.assertEqual(observed['class_path'], HERO)
                self.assertEqual(observed['build_sha256'], 'synthetic')
                self.assertEqual(audit.call_args.args[1], '1090')
                self.assertTrue((Path(result['job']) / 'character_audit.json').is_file())
            finally:
                thread.join(5)

    @unittest.skipUnless(os.name == 'nt' and (BUILD / 'EbonCharacterLoaderSelfTest.exe').is_file(),
                         'build the native character-loader self-test first')
    def test_native_loader_dispatch_and_rejection_paths(self, alive):
        result = subprocess.run([str(BUILD / 'EbonCharacterLoaderSelfTest.exe')],
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('timeout cancellation', result.stdout)
