"""Generate conservative C++ reflection metadata from a verified runtime dump."""

from __future__ import annotations

import hashlib
import json
import os
import re
import tempfile
from pathlib import Path
from typing import Any


class CodegenError(ValueError):
    """The reflection dump is incomplete or internally inconsistent."""


_MAX_OWNER_SIZE = 0x1000000
_MAX_FIELD_SIZE = 0x1000000
_KIND = re.compile(r"^[a-zA-Z][a-zA-Z0-9_]*$")


def _jsonl(path: Path) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    try:
        with path.open("r", encoding="utf-8-sig") as stream:
            for line_number, line in enumerate(stream, 1):
                if not line.strip():
                    continue
                try:
                    row = json.loads(line)
                except json.JSONDecodeError as exc:
                    raise CodegenError(f"{path.name}:{line_number}: {exc.msg}") from exc
                if not isinstance(row, dict):
                    raise CodegenError(f"{path.name}:{line_number}: expected an object")
                rows.append(row)
    except OSError as exc:
        raise CodegenError(f"cannot read {path}: {exc}") from exc
    return rows


def _integer(value: Any, label: str, low: int, high: int) -> int | None:
    if value is None:
        return None
    if isinstance(value, bool):
        raise CodegenError(f"{label} must be an integer")
    try:
        parsed = int(value, 0) if isinstance(value, str) else int(value)
    except (TypeError, ValueError) as exc:
        raise CodegenError(f"{label} must be an integer") from exc
    if parsed < low or parsed > high:
        raise CodegenError(f"{label} is outside [{low}, {high}]")
    return parsed


def _optional_integer(value: Any, label: str, low: int, high: int) -> int | None:
    if value is None or value == "" or value == "unresolved":
        return None
    return _integer(value, label, low, high)


def _cpp_string(value: Any) -> str:
    text = str(value if value is not None else "")
    escaped = []
    for char in text:
        codepoint = ord(char)
        if char in {'"', "\\"}:
            escaped.append("\\" + char)
        elif codepoint < 0x20 or codepoint == 0x7F:
            escaped.extend(f"\\{byte:03o}" for byte in char.encode("utf-8"))
        else:
            escaped.append(char)
    return '"' + "".join(escaped) + '"'


def _symbol(prefix: str, identity: str) -> str:
    readable = re.sub(r"[^A-Za-z0-9_]+", "_", identity).strip("_")[:64]
    if not readable or readable[0].isdigit():
        readable = "_" + readable
    digest = hashlib.sha256(identity.encode("utf-8")).hexdigest()[:12]
    return f"{prefix}{readable}_{digest}"


def _safe_identifier(value: Any, label: str) -> str:
    name = str(value or "")
    if not name or "\x00" in name:
        raise CodegenError(f"{label} is empty or contains NUL")
    return name


def _field_rows(owner: dict[str, Any]) -> list[dict[str, Any]]:
    fields = owner.get("fields", [])
    if not isinstance(fields, list):
        raise CodegenError(f"{owner['path']}: fields must be a list")
    parsed: list[dict[str, Any]] = []
    for index, field in enumerate(fields):
        label = f"{owner['path']}.fields[{index}]"
        if not isinstance(field, dict):
            raise CodegenError(f"{label} must be an object")
        kind = str(field.get("kind") or "property")
        if not _KIND.fullmatch(kind):
            raise CodegenError(f"{label}.kind is invalid")
        name = _safe_identifier(field.get("name"), f"{label}.name")
        offset = _integer(field.get("offset"), f"{label}.offset", -1, _MAX_OWNER_SIZE)
        element_size = _integer(
            field.get("element_size", field.get("size")),
            f"{label}.element_size",
            0,
            _MAX_FIELD_SIZE,
        )
        array_dim = _integer(field.get("array_dim"), f"{label}.array_dim", 0, 65536)
        extent = None
        if offset is not None and offset >= 0 and element_size and array_dim:
            extent = element_size * array_dim
            if extent > _MAX_FIELD_SIZE or offset + extent > _MAX_OWNER_SIZE:
                raise CodegenError(f"{label}: field extent is unreasonable")
        parsed.append(
            {
                "kind": kind,
                "name": name,
                "type": str(field.get("type") or "Unknown"),
                "offset": offset,
                "element_size": element_size,
                "array_dim": array_dim,
                "extent": extent,
                "flags": _optional_integer(field.get("flags"), f"{label}.flags", 0, (1 << 64) - 1),
                "target": field.get("target"),
                "bool_mask": _optional_integer(field.get("bool_mask"), f"{label}.bool_mask", 0, (1 << 64) - 1),
                "role": field.get("role"),
                "address": field.get("address"),
                "detail": field.get("detail"),
                "rep_index": _optional_integer(field.get("rep_index"), f"{label}.rep_index", 0, 65535),
                "rep_condition": _optional_integer(field.get("rep_condition"), f"{label}.rep_condition", 0, 255),
                "rep_notify": field.get("rep_notify"),
            }
        )
    return parsed


def _validate_dump(source: Path) -> tuple[dict[str, Any], list[dict[str, Any]], list[dict[str, Any]]]:
    try:
        state = json.loads((source / "dump_state.json").read_text(encoding="utf-8-sig"))
    except (OSError, json.JSONDecodeError) as exc:
        raise CodegenError(f"cannot read dump_state.json: {exc}") from exc
    if not isinstance(state, dict) or state.get("status") not in {"complete", "complete_with_unresolved"}:
        raise CodegenError("dump_state.json does not report a complete reflection capture")
    if state.get("success") is not True:
        raise CodegenError("dump_state.json success must be true")
    if state.get("errors"):
        raise CodegenError("dump_state.json contains errors")
    if state.get("layout_verified") is not True:
        raise CodegenError("dump_state.json does not confirm validated reflection layout")

    types = _jsonl(source / "types.jsonl")
    objects = _jsonl(source / "objects.jsonl")
    expected_types = _integer(state.get("types"), "dump_state.types", 0, 100_000_000)
    expected_objects = _integer(state.get("objects"), "dump_state.objects", 0, 100_000_000)
    if expected_types is not None and len(types) != expected_types:
        raise CodegenError(f"types.jsonl has {len(types)} rows; state declares {expected_types}")
    if expected_objects is not None and len(objects) != expected_objects:
        raise CodegenError(f"objects.jsonl has {len(objects)} rows; state declares {expected_objects}")

    seen: set[str] = set()
    for index, owner in enumerate(types):
        label = f"types.jsonl[{index}]"
        if state.get("sdk_validated_subset") and owner.get("complete") is False:
            raise CodegenError(f"{label} reports incomplete type metadata")
        path = _safe_identifier(owner.get("path"), f"{label}.path")
        name = _safe_identifier(owner.get("name"), f"{label}.name")
        kind = str(owner.get("kind") or "")
        if kind not in {"class", "struct", "function", "enum"}:
            raise CodegenError(f"{label}.kind is unsupported: {kind!r}")
        if path in seen:
            raise CodegenError(f"duplicate reflected path: {path}")
        seen.add(path)
        owner["path"] = path
        owner["name"] = name
        owner["kind"] = kind
        owner["size"] = _integer(owner.get("size"), f"{label}.size", 0, _MAX_OWNER_SIZE)
        owner["flags"] = _optional_integer(owner.get("flags"), f"{label}.flags", 0, (1 << 64) - 1)
        owner["fields"] = _field_rows(owner)
        enum_values = owner.get("enum_values", [])
        if not isinstance(enum_values, list):
            raise CodegenError(f"{path}: enum_values must be a list")
        parsed_values = []
        for value_index, value in enumerate(enum_values):
            if not isinstance(value, dict):
                raise CodegenError(f"{path}.enum_values[{value_index}] must be an object")
            enum_name = _safe_identifier(value.get("name"), f"{path}.enum_values[{value_index}].name")
            enum_value = _integer(
                value.get("value"), f"{path}.{enum_name}.value", -(1 << 63), (1 << 63) - 1
            )
            if enum_value is None:
                raise CodegenError(f"{path}.{enum_name}.value is missing")
            parsed_values.append({"name": enum_name, "value": enum_value})
        owner["enum_values"] = parsed_values
    return state, types, objects


def _header(owner: dict[str, Any]) -> str:
    symbol = _symbol("Type_", owner["path"])
    size = owner["size"]
    lines = [
        "#pragma once",
        '#include "SDKCore.hpp"',
        "",
        "namespace EbonSDK {",
        f"struct {symbol} final {{",
        f"    static constexpr std::string_view Name = {_cpp_string(owner['name'])};",
        f"    static constexpr std::string_view Path = {_cpp_string(owner['path'])};",
        f"    static constexpr std::string_view SuperPath = {_cpp_string(owner.get('super') or '')};",
        f"    static constexpr std::string_view Kind = {_cpp_string(owner['kind'])};",
        (f"    static constexpr std::uint64_t Flags = 0x{owner['flags']:X}ULL;" if owner["flags"] is not None else "    static constexpr std::uint64_t Flags = 0;"),
        f"    static constexpr std::size_t Size = {size if size is not None else 0};",
        f"    static constexpr bool SizeKnown = {'true' if size is not None and size > 0 else 'false'};",
        "",
    ]
    if size is not None and size > 0:
        lines.extend([f"    std::array<std::byte, {size}> Storage{{}};", ""])
    fields = owner["fields"]
    lines.append(f"    inline static constexpr std::array<FieldInfo, {len(fields)}> Fields{{{{")
    for field in fields:
        offset = field["offset"] if field["offset"] is not None else -1
        element_size = field["element_size"] if field["element_size"] is not None else 0
        array_dim = field["array_dim"] if field["array_dim"] is not None else 0
        extent = field["extent"] if field["extent"] is not None else 0
        layout_known = (
            size is not None
            and size > 0
            and offset >= 0
            and extent > 0
            and offset + extent <= size
        )
        fields_flags = field["flags"]
        flag_text = _cpp_string(f"0x{fields_flags:X}") if fields_flags is not None else '""'
        bool_mask_value = field["bool_mask"] if field["bool_mask"] is not None else -1
        lines.append(
            "        {"
            + ", ".join(
                [
                    _cpp_string(field["name"]),
                    _cpp_string(field["type"]),
                    _cpp_string(field["kind"]),
                    str(offset),
                    str(element_size),
                    str(array_dim),
                    str(extent),
                    flag_text,
                    _cpp_string(field["role"] or ""),
                    _cpp_string(field["target"] or ""),
                    str(bool_mask_value),
                    str(layout_known).lower(),
                ]
            )
            + "},"
        )
    lines.extend(["    }};", ""])
    enum_values = owner["enum_values"]
    lines.append(f"    inline static constexpr std::array<EnumValue, {len(enum_values)}> EnumValues{{{{")
    for value in enum_values:
        lines.append(f"        {{{_cpp_string(value['name'])}, {value['value']}LL}},")
    lines.extend(["    }};", "", "};", "} // namespace EbonSDK", ""])
    return "\n".join(lines)


def generate_sdk(dump_dir: str | os.PathLike[str], output_dir: str | os.PathLike[str]) -> dict[str, Any]:
    """Generate headers and a provenance manifest from a complete reflection dump."""
    source = Path(dump_dir).resolve()
    destination = Path(output_dir).resolve()
    if not source.is_dir():
        raise CodegenError(f"dump directory does not exist: {source}")
    if destination == source or source in destination.parents or destination in source.parents:
        raise CodegenError("SDK output must be separate from the source dump directory")
    if destination.exists() and (not destination.is_dir() or any(destination.iterdir())):
        raise CodegenError(f"SDK output must be new or empty: {destination}")

    state, owners, objects = _validate_dump(source)
    classes = [owner for owner in owners if owner["kind"] in {"class", "struct", "function", "enum"}]
    type_headers = {f"{_symbol('Type_', owner['path'])}.hpp": _header(owner) for owner in classes}
    core = """#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
namespace EbonSDK {
struct FieldInfo {
    std::string_view Name, Type, Kind;
    std::int64_t Offset;
    std::uint32_t ElementSize, ArrayDim, Extent;
    std::string_view Flags, Role, Target;
    std::int64_t BoolMask;
    bool LayoutKnown;
};
struct EnumValue { std::string_view Name; std::int64_t Value; };
}
"""
    index_lines = ['#pragma once', '#include "SDKCore.hpp"']
    for filename in sorted(type_headers):
        index_lines.append(f'#include "Types/{filename}"')
    index_lines.extend(["", "namespace EbonSDK {", "struct TypeIndexEntry { std::string_view Path, Kind; std::size_t Size; bool SizeKnown; };" ])
    index_lines.append(f"inline constexpr std::array<TypeIndexEntry, {len(classes)}> TypeIndex{{{{")
    for owner in classes:
        index_lines.append(
            "    {"
            + ", ".join(
                [
                    _cpp_string(owner["path"]),
                    _cpp_string(owner["kind"]),
                    str(owner["size"] or 0),
                    str(bool(owner["size"])).lower(),
                ]
            )
            + "},"
        )
    index_lines.extend(["}};", "} // namespace EbonSDK", ""])

    unresolved = 0
    invalid_layout = 0
    for owner in classes:
        for field in owner["fields"]:
            size = owner["size"]
            if (
                size is None
                or size <= 0
                or field["offset"] is None
                or field["offset"] < 0
                or not field["extent"]
            ):
                unresolved += 1
            elif field["offset"] + field["extent"] > size:
                invalid_layout += 1

    metadata = {
        "format": 1,
        "generator": "EbonCore-Dumper",
        "source_status": state["status"],
        "source_build": state.get("build"),
        "source_types": len(owners),
        "source_objects": len(objects),
        "generated_types": len(classes),
        "generated_headers": len(type_headers),
        "fields_without_verified_extent": unresolved + invalid_layout,
        "layout_verified": state.get("layout_verified") is True,
        "limitations": [
            "Generated headers are reflection metadata, not ABI-certified C++ object declarations.",
            "Unknown offsets, sizes, flags, pointer targets and container representations remain metadata only.",
            "No functions are emitted as callable wrappers.",
        ],
    }

    destination.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix=".ebon-sdk-", dir=destination))
    try:
        (staging / "Types").mkdir()
        (staging / "SDKCore.hpp").write_text(core, encoding="utf-8", newline="\n")
        (staging / "SDK.hpp").write_text("\n".join(index_lines), encoding="utf-8", newline="\n")
        for name, text in type_headers.items():
            (staging / "Types" / name).write_text(text, encoding="utf-8", newline="\n")
        (staging / "sdk_manifest.json").write_text(
            json.dumps(metadata, ensure_ascii=False, indent=2) + "\n", encoding="utf-8", newline="\n"
        )
        for relative in [Path("SDKCore.hpp"), Path("SDK.hpp"), Path("sdk_manifest.json")]:
            os.replace(staging / relative, destination / relative)
        type_destination = destination / "Types"
        type_destination.mkdir(exist_ok=True)
        for source_file in (staging / "Types").iterdir():
            os.replace(source_file, type_destination / source_file.name)
    finally:
        for path in sorted(staging.rglob("*"), reverse=True):
            if path.is_file():
                path.unlink()
            elif path.is_dir():
                path.rmdir()
        staging.rmdir()
    return metadata
