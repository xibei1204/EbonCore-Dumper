import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from tests.test_cpp_sdk import cpp_fixture
from tools.ebon.sdk_codegen import CodegenError
from tools.ebon.sdk_publish import publish_cpp_sdk
from tools.watch_sdk import publish_pending


class SdkPublishTests(unittest.TestCase):
    def test_locked_directory_retains_complete_candidate_without_changing_existing_sdk(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);source=cpp_fixture(root);output=root/'CppSDK'
            publish_cpp_sdk(source,output);(output/'keep.txt').write_text('in use')
            original=Path.rename
            def rename(path,target):
                if path==output:raise PermissionError('directory is open')
                return original(path,target)
            with patch.object(Path,'rename',new=rename):result=publish_cpp_sdk(source,output)
            self.assertEqual(result['publication_status'],'ready_output_locked')
            self.assertTrue((Path(result['sdk_output'])/'SDK.hpp').is_file())
            self.assertEqual((output/'keep.txt').read_text(),'in use')
            (source/'sdk_hotkey_state.json').write_text(json.dumps(dict(format='EbonSdkHotkey-v1',status='armed',
                last_success=True,runs=1,last_output=str(source))))
            with patch.object(Path,'rename',new=rename):
                publish_pending(source,output,0,lambda *_:None)
            state=json.loads((source/'sdk_body_state.json').read_text())
            self.assertNotEqual(Path(state['sdk_output']),output)
            self.assertTrue((Path(state['sdk_output'])/'SDK.hpp').is_file())

    def test_repeated_publication_keeps_a_backup_and_updates_the_fixed_path(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = cpp_fixture(root)
            output = root / "CppSDK"
            first = publish_cpp_sdk(source, output)
            self.assertIsNone(first["backup_directory"])
            (output / "keep.txt").write_text("previous SDK")
            second = publish_cpp_sdk(source, output)
            self.assertTrue((output / "SDK.hpp").is_file())
            self.assertEqual((Path(second["backup_directory"]) / "keep.txt").read_text(), "previous SDK")
            self.assertFalse((output / "keep.txt").exists())
            self.assertEqual(json.loads((output / "sdk_manifest.json").read_text())["source_capture"], str(source.resolve()))

    def test_failed_generation_leaves_the_existing_sdk_untouched(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = cpp_fixture(root)
            output = root / "CppSDK"
            publish_cpp_sdk(source, output)
            before = (output / "SDK.hpp").read_bytes()
            state_path = source / "dump_state.json"
            state = json.loads(state_path.read_text())
            state.update(status="failed", success=False)
            state_path.write_text(json.dumps(state))
            with self.assertRaises(CodegenError):
                publish_cpp_sdk(source, output)
            self.assertEqual((output / "SDK.hpp").read_bytes(), before)
            self.assertEqual(list(root.glob("CppSDK.previous_*")), [])

    def test_publisher_waits_for_success_and_only_handles_each_run_once(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = cpp_fixture(root)
            output = root / "CppSDK"
            marker = source / "sdk_hotkey_state.json"
            listener = dict(format="EbonSdkHotkey-v1", status="running", runs=1,
                            last_success=False, last_output=str(source))
            marker.write_text(json.dumps(listener))
            logs = []
            log = lambda level, message: logs.append((level, message))
            self.assertEqual(publish_pending(source, output, 0, log), 0)
            self.assertFalse(output.exists())
            listener.update(status="armed", last_success=True)
            marker.write_text(json.dumps(listener))
            self.assertEqual(publish_pending(source, output, 0, log), 1)
            state = json.loads((source / "sdk_body_state.json").read_text())
            self.assertTrue(state["success"])
            self.assertEqual(publish_pending(source, output, 1, log), 1)
            self.assertEqual(list(root.glob("CppSDK.previous_*")), [])

    def test_listener_cannot_publish_a_snapshot_outside_its_capture(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = cpp_fixture(root)
            (source / "sdk_hotkey_state.json").write_text(json.dumps(dict(
                format="EbonSdkHotkey-v1", status="armed", runs=1, last_success=True, last_output=str(root / "other"))))
            self.assertEqual(publish_pending(source, root / "CppSDK", 0, lambda *_: None), 1)
            state = json.loads((source / "sdk_body_state.json").read_text())
            self.assertEqual(state["status"], "failed")
            self.assertFalse((root / "CppSDK").exists())


if __name__ == "__main__":
    unittest.main()
