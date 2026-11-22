import json
import os
from pathlib import Path
import tempfile
import threading
import time
import unittest

from tools.friend_capture import REQUEST, REVISION, STATE, arm_capture, inspect_capture, latest_capture


class FriendCaptureControlTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.state = {'sampler_revision': REVISION, 'capture_scope': 'friends', 'installed': True,
                      'module_sha256': 'fixture', 'window_id': 1, 'window_seconds': 600, 'status': 'armed'}
        self.write_state(self.root)

    def tearDown(self):
        self.temp.cleanup()

    def write_state(self, folder, **values):
        folder.mkdir(parents=True, exist_ok=True)
        (folder / STATE).write_text(json.dumps(self.state | values), encoding='utf-8')

    def test_rearm_waits_for_a_new_acknowledged_window(self):
        def consume():
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline:
                if (self.root / REQUEST).exists():
                    (self.root / REQUEST).unlink()
                    self.write_state(self.root, window_id=2, arm_source='request_file')
                    return
                time.sleep(0.01)
        worker = threading.Thread(target=consume)
        worker.start()
        try:
            result = arm_capture(self.root, 2)
            self.assertEqual(result['window_id'], 2)
            self.assertFalse((self.root / REQUEST).exists())
        finally:
            worker.join(timeout=4)

    def test_timeout_never_reports_the_existing_window_as_acknowledged(self):
        with self.assertRaisesRegex(ValueError, 'No rearm acknowledgement'):
            arm_capture(self.root, 0.02)
        self.assertTrue((self.root / REQUEST).exists())
        with self.assertRaises(FileExistsError):
            arm_capture(self.root, 0.02)

    def test_stale_or_old_sampler_is_rejected_without_writing_a_request(self):
        old = time.time() - 60
        os.utime(self.root / STATE, (old, old))
        with self.assertRaisesRegex(ValueError, 'stale'):
            arm_capture(self.root)
        self.write_state(self.root, sampler_revision='old')
        with self.assertRaisesRegex(ValueError, 'restart'):
            arm_capture(self.root)
        self.assertFalse((self.root / REQUEST).exists())

    def test_auto_selection_ignores_a_newer_gameplay_or_legacy_capture(self):
        self.write_state(self.root / 'friend')
        self.write_state(self.root / 'gameplay', capture_scope='gameplay')
        self.assertEqual(latest_capture(self.root), self.root / 'friend')

    def test_inspect_does_not_promote_a_tick_only_capture(self):
        (self.root / 'game_rpc_samples.jsonl').write_text('', encoding='utf-8')
        result = inspect_capture(self.root)
        self.assertEqual(result['friend_samples'], {'Client_Friend_RPC': 0, 'Server_Friend_RPC': 0})
        self.assertEqual(len(result['missing']), 2)
        self.assertFalse(result['packet_mapping_verified'])

    def test_inspect_preserves_missing_response_and_truncation(self):
        row = {'kind': 'Server_Friend_RPC', 'module_sha256': 'fixture', 'wire_packet_bytes_captured': False,
               'payloads_truncated': True, 'wrappers': [{'custom_type_name': 'HTFriendInfo'}]}
        samples = self.root / 'game_rpc_samples.jsonl'
        samples.write_text(json.dumps(row) + '\n', encoding='utf-8')
        result = inspect_capture(self.root)
        self.assertIn('Client_Friend_RPC_not_observed', result['missing'])
        self.assertIn('truncated_payloads', result['missing'])
        self.assertEqual(result['custom_types'], ['HTFriendInfo'])
        row['module_sha256'] = 'another-build'
        samples.write_text(json.dumps(row) + '\n', encoding='utf-8')
        with self.assertRaisesRegex(ValueError, 'different module'):
            inspect_capture(self.root)

    def test_focused_profile_retains_the_reviewed_metadata_and_rpc_bindings(self):
        root = Path(__file__).resolve().parents[1]
        def read_profile(name):
            return dict(line.split('=', 1) for line in (root / 'profiles' / name).read_text().splitlines()
                        if '=' in line and not line.startswith('#'))
        profile = read_profile('nte-ue5-friend.ini')
        reviewed = read_profile('nte-ue5-gameplay.ini')
        for key, value in reviewed.items():
            if key in ('exe_sha256', 'module') or key.startswith(('name_', 'object_', 'struct_', 'field_',
                    'property_', 'function_', 'process_event_', 'game_rpc_frame_', 'game_rpc_native_')):
                self.assertEqual(profile[key], value, key)
        self.assertEqual(profile['game_rpc_sample_friend_only'], '1')
        self.assertEqual(profile['game_rpc_sample_window_seconds'], '600')
        for key in ('aes_hunt', 'resource_hotkey', 'game_rpc_sync_capture'):
            self.assertEqual(profile[key], '0')


if __name__ == '__main__':
    unittest.main()
