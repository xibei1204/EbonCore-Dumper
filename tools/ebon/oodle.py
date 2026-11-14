"""Optional adapter for an explicitly supplied, trusted Windows x64 Oodle DLL.

Uses the public OodleLZ_Decompress C ABI. Never searches for, downloads, or
redistributes a decoder. Loading a DLL executes its code in this Python process.
"""
from __future__ import annotations

import ctypes
import hashlib
import os
from pathlib import Path

from .containers import ContainerError


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
