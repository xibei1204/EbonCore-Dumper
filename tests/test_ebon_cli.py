from pathlib import Path
import contextlib
import io
from types import SimpleNamespace
import tempfile
import unittest
from unittest.mock import patch
import json

from tests.test_containers import simple_pak
from tools.ebon.sdk_codegen import CodegenError
from tools.ebon_cli import extract_pak, inject_and_dump, wait_for_capture, wait_for_sdk_body, main


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

    def test_f5_wait_shows_the_key_and_starts_a_fresh_capture_timeout(self):
        with tempfile.TemporaryDirectory() as temporary:
            dump = Path(temporary)
            state_path = dump / "dump_state.json"
            state_path.write_text(json.dumps(dict(status="waiting", phase="sdk_hotkey", terminal_available=True)))
            clock = [0.0]
            def advance(_):
                clock[0] = 299.0 if clock[0] == 0 else 301.0
                state_path.write_text(json.dumps(dict(status="collecting") if clock[0] == 299 else dict(status="complete", success=True)))
            output = io.StringIO()
            with patch("tools.ebon_cli.time.monotonic", side_effect=lambda: clock[0]), \
                 patch("tools.ebon_cli.time.sleep", side_effect=advance), contextlib.redirect_stdout(output):
                self.assertTrue(wait_for_capture(dump)["success"])
            self.assertIn("press F5 in the dumper terminal", output.getvalue())

    def test_sdk_native_budget_does_not_time_out_at_the_old_five_minute_limit(self):
        with tempfile.TemporaryDirectory() as temporary:
            dump = Path(temporary)
            state_path = dump / "dump_state.json"
            state_path.write_text(json.dumps(dict(status="waiting", phase="sdk_hotkey")))
            clock = [0.0]
            def advance(_):
                clock[0] = 299.0 if clock[0] == 0 else 800.0
                state = (dict(status="collecting", sdk_validated_subset=True, capture_timeout_seconds=600)
                         if clock[0] == 299 else dict(status="complete", success=True))
                state_path.write_text(json.dumps(state))
            with patch("tools.ebon_cli.time.monotonic", side_effect=lambda: clock[0]), \
                 patch("tools.ebon_cli.time.sleep", side_effect=advance):
                self.assertTrue(wait_for_capture(dump)["success"])

    def test_aes_only_capture_does_not_wait_for_f5(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            injector, dll = root / "injector.exe", root / "dumper.dll"
            injector.touch()
            dll.touch()
            with patch("tools.ebon_cli.subprocess.run", return_value=SimpleNamespace(returncode=0)), \
                 patch("tools.ebon_cli.wait_for_capture") as capture, \
                 patch("tools.ebon_cli.wait_for_aes", return_value={"success": True}):
                self.assertEqual(inject_and_dump(injector, dll, "fixture.exe", root / "profile.ini", root / "dump", None, wait_aes=True), 0)
            capture.assert_not_called()

    def test_default_f5_cli_waits_for_published_sdk_without_generating_a_duplicate(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            injector, dll = root / "injector.exe", root / "dumper.dll"
            injector.touch(); dll.touch()
            profile = root / "profile.ini"
            profile.write_text("sdk_hotkey=1\n")
            capture, published = root / "capture", root / "CppSDK"
            published.mkdir(); (published / "SDK.hpp").write_text("#pragma once\n")
            def inject(_command, **_):
                capture.mkdir()
                (capture / "dump_state.json").write_text(json.dumps(dict(status="complete", success=True)))
                (capture / "sdk_body_state.json").write_text(json.dumps(dict(format="EbonSdkBody-v1", status="complete", success=True, sdk_output=str(published))))
                return SimpleNamespace(returncode=0)
            with patch("tools.ebon_cli.subprocess.run", side_effect=inject), \
                 patch("tools.ebon_cli.generate_cpp_sdk") as generator:
                result = main(["inject-and-dump", "--injector", str(injector), "--dumper", str(dll),
                               "--profile", str(profile), "--dump-out", str(capture)])
                self.assertEqual(result, 0)
                generator.assert_not_called()

    def test_explicit_sdk_output_keeps_the_requested_generation_path(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            profile = root / "profile.ini"
            profile.write_text("sdk_hotkey=1\n")
            requested = root / "custom-sdk"
            with patch("tools.ebon_cli.inject_and_dump", return_value=0) as inject:
                self.assertEqual(main(["inject-and-dump", "--profile", str(profile), "--sdk-out", str(requested)]), 0)
                self.assertEqual(inject.call_args.args[5], requested)
                self.assertFalse(inject.call_args.kwargs["use_hotkey_publisher"])

    def test_sdk_publisher_failure_or_missing_body_is_not_reported_as_success(self):
        with tempfile.TemporaryDirectory() as temporary:
            capture = Path(temporary)
            state = dict(format="EbonSdkBody-v1", status="failed", success=False, error="invalid source")
            (capture / "sdk_body_state.json").write_text(json.dumps(state))
            with self.assertRaisesRegex(CodegenError, "invalid source"):
                wait_for_sdk_body(capture)
            state.update(status="complete", success=True, sdk_output=str(capture / "missing"))
            (capture / "sdk_body_state.json").write_text(json.dumps(state))
            with self.assertRaisesRegex(CodegenError, "SDK.hpp is missing"):
                wait_for_sdk_body(capture)

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
