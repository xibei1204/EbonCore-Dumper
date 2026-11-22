"""Publish each successful F5 snapshot independently of the injection CLI."""
from __future__ import annotations

import argparse
import ctypes
import json
import os
from pathlib import Path
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.ebon.sdk_codegen import CodegenError
from tools.ebon.sdk_publish import publish_cpp_sdk


class ProcessWatch:
    def __init__(self, pid: int):
        self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        self.kernel.OpenProcess.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32]
        self.kernel.OpenProcess.restype = ctypes.c_void_p
        self.kernel.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
        self.kernel.CloseHandle.argtypes = [ctypes.c_void_p]
        self.handle = self.kernel.OpenProcess(0x00100000, False, pid)
        # An inaccessible game is monitored through its log viewer instead.
        self.unobservable = not self.handle and ctypes.get_last_error() == 5

    def alive(self) -> bool:
        return self.unobservable or bool(self.handle and self.kernel.WaitForSingleObject(self.handle, 0) == 0x102)

    def close(self):
        if self.handle:
            self.kernel.CloseHandle(self.handle)
            self.handle = None


def write_state(capture: Path, state: dict):
    temporary = capture / "sdk_body_state.json.tmp"
    temporary.write_text(json.dumps(state, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    for attempt in range(60):
        try:
            temporary.replace(capture / "sdk_body_state.json")
            return
        except PermissionError:
            if attempt == 59:
                raise
            time.sleep(0.025)


def publish_pending(capture: Path, output: Path, previous_run: int, log) -> int:
    try:
        listener = json.loads((capture / "sdk_hotkey_state.json").read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return previous_run
    if (listener.get("format") != "EbonSdkHotkey-v1" or listener.get("status") != "armed"
            or listener.get("last_success") is not True):
        return previous_run
    run = listener.get("runs")
    if not isinstance(run, int) or run <= previous_run:
        return previous_run
    state = {"format": "EbonSdkBody-v1", "status": "generating", "runs": run,
             "publisher_pid": os.getpid(), "sdk_output": str(output), "success": False}
    try:
        source = Path(listener["last_output"])
        if not source.is_absolute():
            source = capture / source
        source = source.resolve()
        if source != capture and (source.parent != capture or not source.name.startswith("sdk_snapshot_")
                                  or not source.name[13:].isdigit() or int(source.name[13:]) < 1):
            raise CodegenError("SDK listener output does not belong to this capture")
        state["source_capture"] = str(source)
        write_state(capture, state)
        log("info", f"F5 C++ SDK generation started: {source}")
        result = publish_cpp_sdk(source, output)
        state.update(status="complete", success=True, backup_directory=result["backup_directory"],
                     sdk_output=result["sdk_output"],requested_sdk_output=str(output),
                     publication_status=result.get("publication_status","published"),
                     generated_layouts=result["manifest"]["generated_layouts"],
                     excluded_types=result["manifest"].get("excluded_types", []))
        write_state(capture, state)
        log("success", f"F5 C++ SDK body ready: {result['sdk_output']} (SDK.hpp, Packages, Runtime.cpp)")
    except Exception as error:
        state.update(status="failed", error=str(error))
        write_state(capture, state)
        log("error", f"F5 C++ SDK generation failed: {error}; the previous SDK is retained")
    return run


def main(argv=None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--capture", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--pid", type=int, required=True)
    parser.add_argument("--owner-pid", type=int, required=True)
    parser.add_argument("--log", type=Path, required=True)
    args = parser.parse_args(argv)
    capture, output = args.capture.resolve(), args.out.resolve()
    def log(level, message):
        with args.log.open("a", encoding="utf-8", newline="") as stream:
            stream.write(f"[{time.strftime('%H:%M:%S')}] [ebon] {level:<7} {message}\n")
    game, viewer = ProcessWatch(args.pid), ProcessWatch(args.owner_pid)
    last_run = 0
    try:
        log("success", f"F5 C++ SDK publisher ready; output={output}")
        while True:
            last_run = publish_pending(capture, output, last_run, log)
            if not game.alive() or not viewer.alive():
                return 0
            time.sleep(0.25)
    finally:
        game.close()
        viewer.close()


if __name__ == "__main__":
    raise SystemExit(main())
