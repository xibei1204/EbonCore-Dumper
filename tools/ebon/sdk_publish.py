"""Generate a complete C++ SDK before replacing the fixed published directory."""
from __future__ import annotations

from contextlib import contextmanager
import ctypes
import hashlib
import json
import os
from pathlib import Path
import tempfile
import time
import uuid

from .cpp_sdk import generate_cpp_sdk
from .sdk_codegen import CodegenError


@contextmanager
def _publish_lock(output: Path):
    if os.name != "nt":
        yield
        return
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateMutexW.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_wchar_p]
    kernel.CreateMutexW.restype = ctypes.c_void_p
    kernel.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    kernel.ReleaseMutex.argtypes = [ctypes.c_void_p]
    kernel.CloseHandle.argtypes = [ctypes.c_void_p]
    key = hashlib.sha256(str(output).casefold().encode("utf-8")).hexdigest()
    handle = kernel.CreateMutexW(None, False, "Local\\EbonSdkPublish_" + key)
    if not handle:
        raise ctypes.WinError(ctypes.get_last_error())
    acquired = False
    try:
        acquired = kernel.WaitForSingleObject(handle, 0) in (0, 0x80)
        if not acquired:
            raise CodegenError("Another SDK generation is publishing to this directory")
        yield
    finally:
        if acquired:
            kernel.ReleaseMutex(handle)
        kernel.CloseHandle(handle)


def publish_cpp_sdk(capture: Path, output: Path, **generation_options) -> dict:
    source, output = Path(capture).resolve(), Path(output).resolve()
    if not output.name or output == output.parent:
        raise CodegenError("SDK output must name a directory")
    if source == output or source in output.parents or output in source.parents:
        raise CodegenError("SDK output must be separate from source dump")
    if output.exists() and not output.is_dir():
        raise CodegenError("SDK output exists as a file")
    output.parent.mkdir(parents=True, exist_ok=True)
    with _publish_lock(output), tempfile.TemporaryDirectory(prefix=f".{output.name}_stage_", dir=output.parent) as temporary:
        stage_root = Path(temporary).resolve()
        if stage_root.parent != output.parent:
            raise CodegenError("SDK staging directory escaped the output parent")
        stage = stage_root / "sdk"
        if (output/'type_map.json').is_file():generation_options.setdefault('symbol_baseline',output/'type_map.json')
        manifest = generate_cpp_sdk(source, stage, **generation_options)
        manifest["source_capture"] = str(source)
        (stage / "sdk_manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        backup = None
        if output.exists():
            backup = output.parent / f"{output.name}.previous_{time.strftime('%Y%m%d_%H%M%S')}_{uuid.uuid4().hex[:8]}"
            if backup.resolve().parent != output.parent:
                raise CodegenError("SDK backup directory escaped the output parent")
            try:
                output.rename(backup)
            except PermissionError:
                # Windows directory viewers/compilers can deny directory deletion
                # even when individual files are readable. Retain the completed
                # candidate rather than destroying minutes of successful generation.
                ready=output.parent/f"{output.name}.ready_{time.strftime('%Y%m%d_%H%M%S')}_{uuid.uuid4().hex[:8]}"
                stage.rename(ready)
                return {"sdk_output":str(ready),"requested_sdk_output":str(output),"backup_directory":None,
                        "publication_status":"ready_output_locked","manifest":manifest}
        try:
            stage.rename(output)
        except Exception:
            if backup is not None and not output.exists():
                backup.rename(output)
            raise
    return {"sdk_output": str(output), "backup_directory": str(backup) if backup else None,
            "publication_status":"published","manifest": manifest}
