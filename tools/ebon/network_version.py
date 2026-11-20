"""Revalidate captured UE NetworkVersion cache evidence without guessing inputs."""
from __future__ import annotations

import json
from pathlib import Path
import re
import struct
import zlib


STATUSES = {"observed_cached", "not_initialized", "unsupported_profile", "disabled",
            "profile_mismatch", "invalid_binding", "unreadable", "signature_mismatch",
            "changed_during_read", "invalid_cache"}


def ue_str_crc32(lowercase_text: str) -> int:
    """This build's StrCrc32 treats each UTF-16 unit as four little-endian bytes.

    Pass already canonicalized text. Python Unicode lowercasing is not claimed
    equivalent to Unreal FChar::ToLower. This is never a cache-value fallback.
    """
    if not isinstance(lowercase_text, str) or "\0" in lowercase_text:
        raise ValueError("CRC input must be a string without embedded NUL")
    encoded = lowercase_text.encode("utf-16-le")
    expanded = b"".join(encoded[at:at+2] + b"\0\0" for at in range(0, len(encoded), 2))
    return zlib.crc32(expanded) & 0xFFFFFFFF


def _integer(value, label, maximum=0xFFFFFFFFFFFFFFFF):
    if isinstance(value, bool):
        raise ValueError(f"{label}: boolean is not an integer")
    if isinstance(value, str):
        value = int(value, 0)
    if not isinstance(value, int) or not 0 <= value <= maximum:
        raise ValueError(f"{label}: invalid integer")
    return value


def validate_network_version(record: dict, expected_sha256: str, module_base=None) -> dict:
    if not isinstance(record, dict) or record.get("format") != "EbonNetworkVersion-v1":
        raise ValueError("unsupported NetworkVersion evidence format")
    if record.get("build_sha256") != expected_sha256:
        raise ValueError("NetworkVersion module SHA-256 differs from its capture")
    status = record.get("status")
    if status not in STATUSES or not isinstance(record.get("verified"), bool):
        raise ValueError("invalid NetworkVersion status")
    if status != "observed_cached":
        if record["verified"] or record.get("value") is not None:
            raise ValueError("unobserved NetworkVersion cannot contain a verified value")
        return dict(status=status, verified=False, value=None, source_build=expected_sha256)
    if record["verified"] is not True or record.get("cache_read_twice") is not True:
        raise ValueError("observed NetworkVersion requires verified double-read evidence")
    if not re.fullmatch(r"[0-9a-f]{64}", expected_sha256 or ""):
        raise ValueError("observed NetworkVersion requires a full module SHA-256")
    base = _integer(record["module_base"], "module_base")
    size = _integer(record["module_size"], "module_size")
    if base < 0x10000 or size == 0 or base + size > 0xFFFFFFFFFFFFFFFF:
        raise ValueError("invalid NetworkVersion module bounds")
    if module_base is not None and base != _integer(module_base, "capture module_base"):
        raise ValueError("NetworkVersion ASLR base differs from its capture")
    getter = _integer(record["getter_rva"], "getter_rva")
    hello = _integer(record["hello_call_rva"], "hello_call_rva")
    flag = _integer(record["cache_flag_rva"], "cache_flag_rva")
    value_rva = _integer(record["cache_value_rva"], "cache_value_rva")
    g = bytes.fromhex(record["getter_bytes"])
    h = bytes.fromhex(record["hello_call_bytes"])
    before = bytes.fromhex(record["cache_bytes"])
    after = bytes.fromhex(record["cache_bytes_after"])
    if len(g) != 24 or len(h) != 9 or len(before) != 8 or len(after) != 8:
        raise ValueError("NetworkVersion evidence byte count mismatch")
    if (g[:3] != b"\x48\x83\xec" or g[4:6] != b"\x80\x3d" or g[10:15] != b"\0\x74\x0b\x8b\x05" or
            g[19:22] != b"\x48\x83\xc4" or g[22] != g[3] or g[23] != 0xC3 or
            h[0] != 0xE8 or h[5:] != b"\x89\x44\x24\x28"):
        raise ValueError("NetworkVersion getter or Hello signature mismatch")
    derived_flag = getter + 11 + struct.unpack_from("<i", g, 6)[0]
    derived_value = getter + 19 + struct.unpack_from("<i", g, 15)[0]
    callee = hello + 5 + struct.unpack_from("<i", h, 1)[0]
    if derived_flag != flag or derived_value != value_rva or value_rva != flag + 4 or callee != getter or value_rva & 3:
        raise ValueError("NetworkVersion RIP targets or Hello callee mismatch")
    for rva, length in ((getter, 24), (hello, 9), (flag, 8)):
        if rva >= size or rva + length > size:
            raise ValueError("NetworkVersion evidence lies outside its module")
    if before != after or before[0] != 1:
        raise ValueError("NetworkVersion cache is uninitialized or changed during read")
    value = _integer(record["value"], "NetworkVersion value", 0xFFFFFFFF)
    if struct.unpack_from("<I", before, 4)[0] != value:
        raise ValueError("NetworkVersion value differs from captured cache bytes")
    return dict(status=status, verified=True, value=value, source_build=expected_sha256,
                module_base=hex(base), getter_rva=hex(getter), hello_call_rva=hex(hello),
                cache_flag_rva=hex(flag), cache_value_rva=hex(value_rva),
                validation="getter_hello_and_stable_cache_bytes_cross_checked", packet_mapping_verified=False)


def checked_network_version(capture: Path, expected_sha256: str | None = None) -> dict:
    path = Path(capture) / "network_version.json"
    if not path.is_file():
        return dict(status="not_observed", verified=False, value=None)
    if path.stat().st_size > 65536:
        raise ValueError("NetworkVersion evidence exceeds 64 KiB")
    state = json.loads((path.parent / "dump_state.json").read_text(encoding="utf-8"))
    digest = (state.get("build") or {}).get("sha256")
    if expected_sha256 is not None and digest != expected_sha256:
        raise ValueError("NetworkVersion source capture SHA-256 mismatch")
    try:
        return validate_network_version(json.loads(path.read_text(encoding="utf-8")), digest, state.get("module_base"))
    except (KeyError, TypeError, struct.error) as error:
        raise ValueError("malformed NetworkVersion evidence: " + str(error)) from error
