"""Runs the actual DLL in an isolated, hidden host; never targets the game."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from tools.ebon.sdk_codegen import generate_sdk

ROOT = Path(__file__).resolve().parents[1]
BUILD = Path(os.environ.get("EBON_TEST_BUILD_DIR", ROOT / "build"))
HOST = BUILD / "EbonDllSelfTest.exe"
DLL = BUILD / "EbonDumper.dll"


@unittest.skipUnless(os.name == "nt" and HOST.is_file() and DLL.is_file(), "build the Windows DLL self-test first")
class DllCaptureTests(unittest.TestCase):
    def run_capture(self, root: Path, mode: str) -> Path:
        dll = root / "EbonDumper.dll"
        shutil.copy2(DLL, dll)
        output = root / "capture"
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0
        result = subprocess.run(
            [str(HOST), mode, str(dll), str(output)],
            capture_output=True, text=True, timeout=15,
            startupinfo=startup, creationflags=subprocess.CREATE_NEW_CONSOLE,
        )
        log = (root / "fixture.log").read_text(encoding="utf-8") if (root / "fixture.log").is_file() else ""
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr + log)
        return output

    def test_dll_discovers_collects_and_generates_sdk(self):
        with tempfile.TemporaryDirectory(prefix="ebon-dll-complete-") as temporary:
            root = Path(temporary)
            output = self.run_capture(root, "complete")
            state = json.loads((output / "dump_state.json").read_text(encoding="utf-8"))
            self.assertTrue(state["success"])
            self.assertEqual(state["objects"], 6)
            self.assertGreater(state["properties"], 0)
            generate_sdk(output, root / "sdk")
            self.assertTrue((root / "sdk" / "SDK.hpp").is_file())

    def test_dll_missing_layout_times_out_with_diagnostics(self):
        with tempfile.TemporaryDirectory(prefix="ebon-dll-missing-") as temporary:
            output = self.run_capture(Path(temporary), "missing")
            state = json.loads((output / "dump_state.json").read_text(encoding="utf-8"))
            self.assertEqual(state["status"], "failed")
            self.assertFalse(state["success"])
            self.assertTrue(any("found 0" in error for error in state["errors"]))
            self.assertFalse((output / "objects.jsonl").exists())

    def test_dll_retries_until_property_registration_finishes(self):
        with tempfile.TemporaryDirectory(prefix="ebon-dll-registration-") as temporary:
            root = Path(temporary)
            output = self.run_capture(root, "delayed")
            state = json.loads((output / "dump_state.json").read_text(encoding="utf-8"))
            self.assertTrue(state["success"])
            self.assertGreater(state["properties"], 0)
            self.assertIn("metadata is not ready", (root / "fixture.log").read_text(encoding="utf-8"))


    def test_resource_watcher_repeats_without_overwriting_startup_capture(self):
        with tempfile.TemporaryDirectory(prefix="ebon-dll-resources-") as temporary:
            output = self.run_capture(Path(temporary), "resources")
            startup = json.loads((output / "dump_state.json").read_text(encoding="utf-8"))
            self.assertTrue(startup["success"])
            for serial in (1, 2):
                capture = output / "resources" / f"res_{serial}"
                state = json.loads((capture / "resource_state.json").read_text(encoding="utf-8"))
                self.assertEqual(state["status"], "no_resources")
                self.assertFalse(state["success"])
                self.assertEqual(state["resources"], 0)
                self.assertTrue((capture / "reflection" / "objects.jsonl").is_file())
                self.assertTrue((capture / "character_skins" / "resources.jsonl").is_file())
            self.assertFalse((output / "resource_capture.request").exists())


if __name__ == "__main__":
    unittest.main()
