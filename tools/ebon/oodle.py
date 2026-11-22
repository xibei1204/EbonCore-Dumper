"""Bundled Oodle decoding in a child process, plus explicit local DLL support."""
from __future__ import annotations

import ctypes
import hashlib
import os
import subprocess
from pathlib import Path

from .containers import ContainerError


class OpenSourceOodleDecoder:
    """Run the bundled GPL decoder in a bounded child process."""
    def __init__(self, path: Path | None = None):
        self.path = (path or Path(__file__).resolve().parents[2] / 'build' / 'EbonOodleDecode.exe').resolve()
        if not self.path.is_file():
            raise ContainerError('Oodle decoder is missing; run build_native.bat or supply --oodle-dll')
        self.provenance = {'kind': 'bundled_ooz_process', 'path': str(self.path),
                           'sha256': hashlib.sha256(self.path.read_bytes()).hexdigest()}

    def __call__(self, method: str, data: bytes, expected: int) -> bytes:
        if method.casefold() != 'oodle' or not 0 <= expected <= 512*1024*1024 or len(data) > 512*1024*1024:
            raise ContainerError('unsupported method or oversized Oodle block')
        try:
            result = subprocess.run([str(self.path), '--size', str(expected)], input=data,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30,
                creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
        except (OSError, subprocess.TimeoutExpired) as error:
            raise ContainerError('Oodle decoder could not complete the block') from error
        if result.returncode or len(result.stdout) != expected:
            raise ContainerError('Oodle decoder rejected the stream or its decoded size')
        return result.stdout


def bundled_decoder():
    path = Path(__file__).resolve().parents[2] / 'build' / 'EbonOodleDecode.exe'
    return OpenSourceOodleDecoder(path) if path.is_file() else None


class OodleDecoder:
    def __init__(self, path: Path, expected_sha256: str | None = None):
        if os.name != "nt" or ctypes.sizeof(ctypes.c_void_p) != 8:
            raise ContainerError("Oodle DLL adapter requires Windows x64 Python")
        path = path.resolve(strict=True)
        if path.suffix.casefold() != ".dll" or not 0 < path.stat().st_size <= 128 * 1024 * 1024:
            raise ContainerError("expected a local Oodle DLL no larger than 128 MiB")
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        if expected_sha256 is not None and digest != expected_sha256.casefold():
            raise ContainerError("Oodle DLL SHA-256 mismatch; decoder not loaded")
        try:
            # DLL directory + System32 only; do not resolve dependencies via cwd.
            self.library = ctypes.CDLL(str(path), winmode=0x1100)
            self.function = self.library.OodleLZ_Decompress
        except (OSError, AttributeError) as error:
            raise ContainerError("cannot load compatible OodleLZ_Decompress export") from error
        ptr, length, integer = ctypes.c_void_p, ctypes.c_ssize_t, ctypes.c_int
        self.function.argtypes = [ptr, length, ptr, length, integer, integer, integer,
                                  ptr, length, ptr, ptr, ptr, length, integer]
        self.function.restype = length
        self.provenance = {"kind": "user_supplied_oodle_dll", "path": str(path), "sha256": digest,
                           "compatibility": "requires_validation_with_target_blocks"}

    def __call__(self, method: str, data: bytes, expected: int) -> bytes:
        if method.casefold() != "oodle":
            raise ContainerError(f"Oodle adapter cannot decode method {method!r}")
        if not 0 <= expected <= 4096 * 1024 * 1024:
            raise ContainerError("Oodle output size outside supported bounds")
        if expected == 0:
            if data:
                raise ContainerError("nonempty Oodle block with empty output")
            return b""
        source = ctypes.create_string_buffer(data)
        output = ctypes.create_string_buffer(expected)
        # FuzzSafe=Yes, CheckCRC=No (not all UE streams contain Oodle CRC),
        # Verbosity=None, DecodeThreadPhase=All; no callback/owned decoder memory.
        written = self.function(source, len(data), output, expected, 1, 0, 0,
                                None, 0, None, None, None, 0, 3)
        if written != expected:
            raise ContainerError(f"Oodle decompressed size mismatch ({written} != {expected})")
        return output.raw
