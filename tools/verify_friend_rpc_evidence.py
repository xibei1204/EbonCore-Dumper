"""Read-only cross-check of Friend RPC class-cache indices against captured bytes.

This verifies runtime memory records, not packet framing or submessage encoding.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from tools.ebon.runtime_mapping import checked_runtime_mapping
from tools.ebon.runtime_network import CapturedMemory, RuntimeReplay
from tools.ebon.sdk_codegen import CodegenError, _jsonl, _validate_dump


OWNER = "/Script/HTGame.HTPlayerState"
TARGETS = {OWNER + ".Client_Friend_RPC": ("client", 0x1000000),
           OWNER + ".Server_Friend_RPC": ("server", 0x200000)}


def _read_json(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def _number(value):
    return int(value, 0) if isinstance(value, str) else int(value)


def _signature(owner):
    keys = ("name", "type", "offset", "element_size", "array_dim", "target")
    return [(tuple(field.get(key) for key in keys), _number(field.get("flags") or 0))
            for field in owner.get("fields", [])]


def verify(reflection: Path, network: Path, expected_sha256: str) -> dict:
    """Return evidence without creating or changing any source file."""
    reflection, network = Path(reflection).resolve(), Path(network).resolve()
    expected_sha256 = expected_sha256.lower()
    if not re.fullmatch(r"[0-9a-f]{64}", expected_sha256):
        raise CodegenError("expected SHA-256 must contain 64 hexadecimal digits")
    state, baseline, _ = _validate_dump(reflection)
    scene = _read_json(network / "dump_state.json")
    for label, value in (("reflection", state), ("network", scene)):
        if (value.get("build") or {}).get("sha256", "").lower() != expected_sha256:
            raise CodegenError(label + " capture module SHA-256 mismatch")
    owners = _jsonl(network / "types.jsonl")
    by_path = {}
    for owner in owners:
        if owner["path"] in by_path:
            raise CodegenError("duplicate network reflected path")
        by_path[owner["path"]] = owner
    references = {owner["path"]: owner for owner in baseline}
    for path in (OWNER, *TARGETS):
        current, reference = by_path.get(path), references.get(path)
        if not current or not reference or current.get("complete") is False:
            raise CodegenError(path + " lacks complete matching reflection evidence")
        if (current.get("kind"), current.get("super")) != (reference.get("kind"), reference.get("super")):
            raise CodegenError(path + " owner kind/inheritance changed")
        if path not in TARGETS:
            continue
        flags = _number(current.get("flags") or 0)
        if flags != _number(reference.get("flags") or 0) or not flags & 0x40 or not flags & 0x80:
            raise CodegenError(path + " RPC flags differ from verified reflection")
        if [bit for bit in (0x1000000, 0x200000, 0x4000) if flags & bit] != [TARGETS[path][1]]:
            raise CodegenError(path + " RPC direction is inconsistent")
        if _signature(current) != _signature(reference):
            raise CodegenError(path + " parameter layout changed")
    metadata = _read_json(network / "network_metadata.json")
    if metadata.get("format") != "EbonNetwork-v1":
        raise CodegenError("unsupported network metadata format")
    active = metadata.get("active_network_objects", [])
    objects = {_number(obj["address"]): obj for obj in _jsonl(network / "objects.jsonl")}
    for item in active:
        obj = objects.get(_number(item["address"]))
        if not obj or obj.get("path") != item.get("path") or obj.get("class") != item.get("class"):
            raise CodegenError("active network object differs from captured object record")
    mapping_path = network / "runtime_mapping.json"
    if mapping_path.is_file():
        raw = _read_json(mapping_path)
        evidence_source = "runtime_mapping_revalidated_against_memory"
    elif metadata.get("runtime_scan", {}).get("graph_retained") is False:
        raw = dict(format="EbonRuntimeMapping-v1", capture=str(network),
                   module_sha256=expected_sha256,
                   class_net_caches=metadata.get("runtime_cache_records", []),
                   rep_layouts=metadata.get("rep_layouts", []))
        evidence_source = "native_records_revalidated_against_memory"
    else:
        raw = RuntimeReplay(network).scan()
        evidence_source = "captured_memory_replayed_in_memory"
    # Always supply raw_mapping: the helper's default path can save a replay.
    verified = checked_runtime_mapping(network, owners, expected_sha256, active, raw_mapping=raw)
    memory = CapturedMemory(network)
    targets = []
    for path, (direction, _) in TARGETS.items():
        matches = [row for row in verified["field_indices"] if row["path"] == path]
        if not matches:
            raise CodegenError(path + " was not observed in a validated class cache")
        if len({(row["field_net_index"], row["checksum"]) for row in matches}) != 1:
            raise CodegenError(path + " has conflicting validated cache observations")
        observations = []
        for row in matches:
            if row.get("incompatible") or not row.get("rpc") or row.get("class") != OWNER:
                raise CodegenError(path + " class cache is incompatible")
            cache = next(cache for cache in verified["caches"] if cache["address"] == row["cache_address"])
            observations.append(dict(cache_address=row["cache_address"],
                                     cache_header_bytes=memory.read(_number(cache["address"]), 48).hex(),
                                     record_address=row["record_address"],
                                     record_size=cache["field_stride"],
                                     record_bytes=memory.read(_number(row["record_address"]), cache["field_stride"]).hex(),
                                     function_address=by_path[path]["address"]))
        targets.append(dict(path=path, direction=direction, field_net_index=matches[0]["field_net_index"],
                            checksum=matches[0]["checksum"], observations=observations))
    source_files = [(reflection / name) for name in ("dump_state.json", "types.jsonl", "objects.jsonl")]
    source_files += [(network / name) for name in ("dump_state.json", "types.jsonl", "objects.jsonl",
                                                  "network_metadata.json", "network_memory.json", "network_memory.bin")]
    if mapping_path.is_file():
        source_files.append(mapping_path)
    digests = []
    for source in dict.fromkeys(source_files):
        with source.open("rb") as stream:
            sha = hashlib.file_digest(stream, "sha256").hexdigest()
        digests.append(dict(path=str(source), sha256=sha))
    return dict(format="EbonFriendRpcEvidence-v1", module_sha256=expected_sha256,
                reflection_capture=str(reflection), network_capture=str(network),
                reflection_capture_success=True, network_capture_success=scene.get("success") is True,
                network_capture_errors=len(scene.get("errors", [])), evidence_source=evidence_source,
                field_indices_verified=True, packet_mapping_verified=False,
                wire_packet_bytes_captured=False, submessage_encoding_verified=False,
                rpcs=targets, source_files=digests,
                unresolved=["Friend RPC packet bytes and class-cache index correlation",
                            "FHottaRpcMessage.Type values and BinaryData wire encoding"])


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reflection", type=Path, required=True)
    parser.add_argument("--network", type=Path, required=True)
    parser.add_argument("--expected-sha256", required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args(argv)
    output = args.out.resolve()
    for source in (args.reflection.resolve(), args.network.resolve()):
        if output == source or source in output.parents:
            parser.error("report output must be outside both source captures")
    if output.exists():
        parser.error("report output must be a new file")
    try:
        result = verify(args.reflection, args.network, args.expected_sha256)
        output.parent.mkdir(parents=True, exist_ok=True)
        with output.open("x", encoding="utf-8") as stream:
            json.dump(result, stream, ensure_ascii=False, indent=2)
            stream.write("\n")
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(1, str(error) + "\n")
    print(json.dumps(dict(output=str(output), field_indices_verified=True,
                          packet_mapping_verified=False), indent=2))
    return 0


if __name__ == "__main__":
    main()
