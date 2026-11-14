from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import json

from tests.test_containers import simple_pak
from tools.ebon.sdk_codegen import CodegenError
from tools.ebon_cli import extract_pak, inject_and_dump, wait_for_capture


class PakCliTests(unittest.TestCase):
    def test_extracts_selected_entries_and_writes_manifest(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            client = root / "client"
            client.mkdir()
            pak = client / "test.pak"
            simple_pak(pak)
            output = root / "out"
            state = extract_pak(pak, output, "MISSING_EBON_KEY", ["content/*.uasset"], 10, 2, 4)
            self.assertEqual(state["status"], "complete")
            self.assertEqual(state["extracted"], 1)
            self.assertEqual((output / "Content" / "Test.uasset").read_bytes(), b"sample asset")
            self.assertTrue((output / "pak_extraction.jsonl").is_file())

    def test_encrypted_real_or_synthetic_pak_reports_missing_key(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            client = root / "client"
            client.mkdir()
            pak = client / "encrypted.pak"
            simple_pak(pak, encrypted=True)
            state = extract_pak(pak, root / "out", "MISSING_EBON_KEY", ["*"], 10, 2, 4)
            self.assertEqual(state["status"], "complete_with_unresolved")
            self.assertEqual(state["extracted"], 0)
            self.assertIn("verified AES", state["unresolved"][0]["reason"])

    def test_refuses_output_inside_client_installation_and_existing_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            install = root / "Neverness To Everness"
            client = install / "Client" / "Content"
            client.mkdir(parents=True)
            pak = client / "test.pak"
            simple_pak(pak)
            with self.assertRaisesRegex(CodegenError, "outside the read-only client"):
                extract_pak(pak, install / "output", "MISSING_EBON_KEY", ["*"], 10, 2, 4)
            existing = root / "out"
            existing.mkdir()
            (existing / "keep.txt").write_text("preserved", encoding="utf-8")
            with self.assertRaisesRegex(CodegenError, "new or empty"):
                extract_pak(pak, existing, "MISSING_EBON_KEY", ["*"], 10, 2, 4)
            self.assertEqual((existing / "keep.txt").read_text(encoding="utf-8"), "preserved")

    def test_inject_and_dump_validates_paths(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            missing_exe = root / "missing_inject.exe"
            missing_dll = root / "missing_dumper.dll"
            dump_out = root / "out"
            with self.assertRaisesRegex(CodegenError, "injector executable does not exist"):
                inject_and_dump(missing_exe, missing_dll, "HTGame.exe", root / "p.ini", dump_out, None)
            fake_exe = root / "fake.exe"
            fake_exe.write_text("x")
            with self.assertRaisesRegex(CodegenError, "dumper DLL does not exist"):
                inject_and_dump(fake_exe, missing_dll, "HTGame.exe", root / "p.ini", dump_out, None)

    def test_capture_waits_past_progress_states(self):
        with tempfile.TemporaryDirectory() as temporary:
            dump = Path(temporary)
            state_path = dump / "dump_state.json"
            state_path.write_text(json.dumps({"status": "discovering", "success": False}))

            def finish(_):
                state_path.write_text(json.dumps({"status": "complete", "success": True}))

            with patch("tools.ebon_cli.time.sleep", side_effect=finish):
                self.assertTrue(wait_for_capture(dump)["success"])

    def test_capture_failure_is_terminal_and_missing_worker_times_out(self):
        with tempfile.TemporaryDirectory() as temporary:
            dump = Path(temporary)
            (dump / "dump_state.json").write_text(json.dumps({"status": "failed", "success": False, "errors": ["found 0"]}))
            self.assertEqual(wait_for_capture(dump)["errors"], ["found 0"])
            with patch("tools.ebon_cli.time.monotonic", side_effect=[0.0, 301.0]):
                with self.assertRaisesRegex(CodegenError, "did not finish"):
                    wait_for_capture(dump)


if __name__ == "__main__":
    unittest.main()
