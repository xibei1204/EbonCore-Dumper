"""Runs the actual DLL in an isolated, hidden host; never targets the game."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

from tools.ebon.sdk_codegen import generate_sdk

ROOT = Path(__file__).resolve().parents[1]
BUILD = Path(os.environ.get("EBON_TEST_BUILD_DIR", ROOT / "build"))
HOST = BUILD / "EbonDllSelfTest.exe"
DLL = BUILD / "EbonDumper.dll"
CONSOLE = BUILD / "EbonLogConsole.exe"

KEY_INPUT_SCRIPT = r'''
import ctypes
from ctypes import wintypes as w
import json
import sys
import time
k = ctypes.WinDLL("kernel32", use_last_error=True)
class Key(ctypes.Structure):
    _fields_ = [("down", w.BOOL), ("repeat", w.WORD), ("vk", w.WORD),
                ("scan", w.WORD), ("char", w.WCHAR), ("control", w.DWORD)]
class Event(ctypes.Union):
    _fields_ = [("key", Key), ("padding", ctypes.c_byte * 16)]
class Record(ctypes.Structure):
    _fields_ = [("kind", w.WORD), ("event", Event)]
k.AttachConsole.argtypes = [w.DWORD]
k.CreateFileW.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, w.LPVOID, w.DWORD, w.DWORD, w.HANDLE]
k.CreateFileW.restype = w.HANDLE
k.WriteConsoleInputW.argtypes = [w.HANDLE, ctypes.POINTER(Record), w.DWORD, ctypes.POINTER(w.DWORD)]
k.CloseHandle.argtypes = [w.HANDLE]
class Coord(ctypes.Structure):
    _fields_ = [("x", w.SHORT), ("y", w.SHORT)]
k.ReadConsoleOutputCharacterW.argtypes = [w.HANDLE, w.LPWSTR, w.DWORD, Coord, ctypes.POINTER(w.DWORD)]
k.FreeConsole()
if not k.AttachConsole(int(sys.argv[1])):
    raise ctypes.WinError(ctypes.get_last_error())
h = k.CreateFileW("CONIN$", 0xC0000000, 3, None, 3, 0, None)
if h == w.HANDLE(-1).value:
    raise ctypes.WinError(ctypes.get_last_error())
records = (Record * 5)()
for row, (vk, down) in zip(records, [(0x74, True), (0x74, True), (0x74, False), (0x78, True), (0x78, False)]):
    row.kind = 1
    row.event.key = Key(down, 4 if down else 1, vk, 0, "\0", 0)
written = w.DWORD()
if not k.WriteConsoleInputW(h, records, len(records), ctypes.byref(written)) or written.value != len(records):
    raise ctypes.WinError(ctypes.get_last_error())
k.CloseHandle(h)
h = k.CreateFileW("CONOUT$", 0x80000000, 3, None, 3, 0, None)
screen = ctypes.create_unicode_buffer(32768)
deadline = time.monotonic() + 5
while True:
    if not k.ReadConsoleOutputCharacterW(h, screen, len(screen) - 1, Coord(0, 0), ctypes.byref(written)):
        raise ctypes.WinError(ctypes.get_last_error())
    text = screen[:written.value]
    if "F5 queued" in text and "F9 queued" in text:
        print(json.dumps({"f5": text.count("F5 queued"), "f9": text.count("F9 queued")}))
        break
    if time.monotonic() >= deadline:
        raise RuntimeError("Console did not display both hotkey requests")
    time.sleep(0.05)
k.CloseHandle(h)
k.FreeConsole()
'''


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

    def test_sdk_waits_for_f5_and_repeat_keeps_the_first_capture(self):
        with tempfile.TemporaryDirectory(prefix="ebon-dll-sdk-hotkey-") as temporary:
            root = Path(temporary)
            output = self.run_capture(root, "sdk_hotkey")
            listener = json.loads((output / "sdk_hotkey_state.json").read_text())
            self.assertEqual(listener["status"], "armed")
            self.assertEqual(listener["key"], "F5")
            self.assertEqual(listener["runs"], 2)
            self.assertTrue(listener["last_success"])
            for capture, sdk in ((output, root / "sdk"), (output / "sdk_snapshot_1", root / "sdk-repeat")):
                generate_sdk(capture, sdk)
                self.assertTrue((sdk / "SDK.hpp").is_file())
            self.assertFalse((output / "sdk_snapshot_2").exists())

    def test_failed_f5_capture_can_retry_without_reinjecting(self):
        with tempfile.TemporaryDirectory(prefix="ebon-dll-sdk-retry-") as temporary:
            output = self.run_capture(Path(temporary), "sdk_hotkey_retry")
            failure = json.loads((output / "dump_state.failed_1.json").read_text())
            self.assertFalse(failure["success"])
            listener = json.loads((output / "sdk_hotkey_state.json").read_text())
            self.assertEqual(listener["runs"], 3)
            self.assertTrue(listener["last_success"])

    @unittest.skipUnless(CONSOLE.is_file(), "build the log console first")
    def test_log_console_generates_and_updates_cpp_sdk_body_without_injection_cli(self):
        with tempfile.TemporaryDirectory(prefix="ebon-sdk-body-") as temporary:
            root = Path(temporary)
            output = self.run_capture(root, "sdk_hotkey")
            sdk = root / "CppSDK"
            log = root / "publisher.log"
            startup = subprocess.STARTUPINFO()
            startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
            startup.wShowWindow = 0
            viewer = subprocess.Popen([str(CONSOLE), "--pid", str(os.getpid()), "--log", str(log),
                                       "--request-dir", str(output), "--sdk-output", str(sdk)],
                                      startupinfo=startup, creationflags=subprocess.CREATE_NEW_CONSOLE)
            publisher_pid = None
            def wait_body(run):
                nonlocal publisher_pid
                deadline = time.monotonic() + 20
                while time.monotonic() < deadline:
                    try:
                        state = json.loads((output / "sdk_body_state.json").read_text())
                        publisher_pid = state["publisher_pid"]
                        if state.get("runs") == run and state["status"] in {"complete", "failed"}:
                            self.assertTrue(state["success"], state)
                            return state
                    except (OSError, json.JSONDecodeError):
                        pass
                    time.sleep(0.05)
                self.fail("C++ SDK body publisher did not finish: " + (log.read_text() if log.exists() else "no publisher log"))
            try:
                wait_body(2)
                for name in ("SDK.hpp", "Types.hpp", "Runtime.cpp", "sdk_manifest.json"):
                    self.assertTrue((sdk / name).is_file())
                (sdk / "keep.txt").write_text("previous output")
                listener_path = output / "sdk_hotkey_state.json"
                listener = json.loads(listener_path.read_text())
                listener.update(runs=3, last_output=str(output))
                listener_path.write_text(json.dumps(listener))
                state = wait_body(3)
                self.assertEqual((Path(state["backup_directory"]) / "keep.txt").read_text(), "previous output")
                self.assertEqual(json.loads((sdk / "sdk_manifest.json").read_text())["source_capture"], str(output))
            finally:
                viewer.terminate()
                viewer.wait(timeout=5)
                if publisher_pid:
                    from tools.watch_sdk import ProcessWatch
                    publisher = ProcessWatch(publisher_pid)
                    try:
                        deadline = time.monotonic() + 5
                        while publisher.alive() and time.monotonic() < deadline:
                            time.sleep(0.05)
                    finally:
                        publisher.close()

    @unittest.skipUnless(CONSOLE.is_file(), "build the log console first")
    def test_terminal_routes_f5_and_f9_and_ignores_held_key_repeats(self):
        with tempfile.TemporaryDirectory(prefix="ebon-console-hotkeys-") as temporary:
            root = Path(temporary)
            startup = subprocess.STARTUPINFO()
            startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
            startup.wShowWindow = 0
            viewer = subprocess.Popen(
                [str(CONSOLE), "--pid", str(os.getpid()), "--log", str(root / "console.log"),
                 "--request-dir", str(root)], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                startupinfo=startup, creationflags=subprocess.CREATE_NEW_CONSOLE,
            )
            try:
                # Wait for the viewer to own its console input before writing events.
                time.sleep(0.5)
                result = subprocess.run([sys.executable, "-c", KEY_INPUT_SCRIPT, str(viewer.pid)],
                                        capture_output=True, text=True, timeout=5,
                                        creationflags=subprocess.CREATE_NO_WINDOW)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                deadline = time.monotonic() + 5
                while not ((root / "sdk_dump.request").is_file() and (root / "aes_hunt.request").is_file()):
                    self.assertIsNone(viewer.poll(), "log console exited before queuing the hotkeys")
                    self.assertLess(time.monotonic(), deadline, "F5/F9 did not queue their request files")
                    time.sleep(0.05)
                time.sleep(0.1)
            finally:
                viewer.terminate()
                stdout, stderr = viewer.communicate(timeout=5)
            counts = json.loads(result.stdout)
            self.assertEqual(counts, {"f5": 1, "f9": 1}, stdout + stderr)

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
