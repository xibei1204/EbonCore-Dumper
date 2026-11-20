"""Export existing F10 payloads as separate files, leaving indexes metadata-only."""
from __future__ import annotations

from contextlib import ExitStack
import hashlib
import io
import json
from pathlib import Path
import time

from .containers import ContainerError, _relative_path, _write_output
from .resource_bundle import BundleLimits


CATEGORIES = (
    "characters", "gacha", "activities_systems", "story_quests", "localization",
    "character_skins", "maps", "scripts", "items_economy", "combat_ai",
    "protocols", "blueprints", "other_assets",
)


def _json_lines(stream, maximum: int):
    while raw := stream.readline(maximum + 1):
        if len(raw) > maximum:
            raise ContainerError("resource JSONL line exceeds size limit")
        if not raw.strip():
            continue
        try:
            value = json.loads(raw)
        except (ValueError, RecursionError) as error:
            raise ContainerError("invalid resource JSONL") from error
        if not isinstance(value, dict):
            raise ContainerError("resource JSONL record must be an object")
        yield value


def _unresolved_count(value: dict) -> int:
    pending, count = [value], 0
    while pending:
        item = pending.pop()
        if isinstance(item, dict):
            count += "_unresolved" in item
            pending.extend(item.values())
        elif isinstance(item, list):
            pending.extend(item)
    return count


def _body_path(identity: str) -> str:
    if not identity.startswith("/") or "\\" in identity:
        raise ContainerError(f"invalid resource object path: {identity!r}")
    package, *objects = identity[1:].split(".")
    if "/" not in package or package.endswith("/") or any("/" in name for name in objects):
        raise ContainerError(f"invalid resource object path: {identity!r}")
    package_parts = package.split("/")
    if package_parts[0] == "Game":
        package_parts = package_parts[1:]
    if not package_parts:
        raise ContainerError(f"invalid resource object path: {identity!r}")
    package = "/".join(package_parts)
    for name in package_parts:
        _relative_path(name)
        if name.upper() in {"CONIN$", "CONOUT$", "COM\u00b9", "COM\u00b2", "COM\u00b3", "LPT\u00b9", "LPT\u00b2", "LPT\u00b3"}:
            raise ContainerError(f"reserved device name in resource object path: {identity!r}")
    # Keep nonmatching exports and subobjects distinct from their package asset.
    package_name = package.rsplit("/", 1)[-1]
    if objects and objects[0] == package_name:
        objects = objects[1:]
    safe_objects = []
    for name in objects:
        safe_name = name.replace(":", "%3A")
        _relative_path(safe_name)
        safe_objects.append(safe_name)
    suffix = "".join("." + name for name in safe_objects)
    return f"data/{package}{suffix}.jsonl"


class ResourceBodyExporter:
    def __init__(self, capture: Path, output: Path, limits: BundleLimits | None = None):
        self.capture, self.output = capture.resolve(), output.resolve()
        self.limits = limits or BundleLimits(max_total_mb=4096, max_manifest_mb=64)
        self.limits.validate()
        if not self.capture.is_dir():
            raise ContainerError("resource capture directory does not exist")
        if self.output == self.capture or self.output.is_relative_to(self.capture) or self.capture.is_relative_to(self.output):
            raise ContainerError("body output must be separate from the read-only capture")
        if self.output.exists() and (not self.output.is_dir() or any(self.output.iterdir())):
            raise ContainerError("body output must be new or empty")
        self.source_state = self.read_state()
        self.deadline = time.monotonic() + self.limits.timeout_seconds
        self.resources = self.copies = self.payload_bytes = self.metadata_bytes = self.recovered = self.partial_resources = 0
        self.unresolved = 0
        self.errors: list[dict] = []
        self.state_written = False
        self.category_counts = {name: 0 for name in CATEGORIES}
        self.body_paths: dict[str, str] = {}

    def source_path(self, relative: str) -> Path:
        path = self.capture / _relative_path(relative)
        resolved = path.resolve()
        if not resolved.is_relative_to(self.capture) or path.is_symlink() or resolved != path:
            raise ContainerError("linked or escaping resource path is not read")
        return path

    def read_state(self) -> dict:
        path = self.source_path("resource_state.json")
        with path.open("rb") as stream:
            data = stream.read(1024 * 1024 + 1)
        if len(data) > 1024 * 1024:
            raise ContainerError("capture state exceeds 1 MiB")
        try:
            state = json.loads(data)
        except (ValueError, RecursionError) as error:
            raise ContainerError("invalid capture state") from error
        if not isinstance(state, dict) or state.get("format") != "EbonResources-v1":
            raise ContainerError("expected an EbonResources-v1 F10 capture")
        if state.get("status") not in {"complete", "complete_with_unresolved", "failed", "no_resources"}:
            raise ContainerError("capture is still running or has an unknown status")
        return state

    def checkpoint(self):
        if time.monotonic() >= self.deadline:
            raise ContainerError("resource body export deadline exceeded")

    def line(self, stream, record: dict):
        self.checkpoint()
        data = (json.dumps(record, ensure_ascii=False, separators=(",", ":")) + "\n").encode("utf-8")
        if self.metadata_bytes + len(data) > self.limits.max_manifest_mb * 1024 * 1024:
            raise ContainerError("resource body metadata byte budget exceeded")
        stream.write(data)
        stream.flush()
        self.metadata_bytes += len(data)

    def payload(self, relative: str):
        path = self.source_path(relative)
        before = path.stat()
        maximum = self.limits.max_file_mb * 1024 * 1024
        if before.st_size > maximum:
            raise ContainerError("resource body exceeds file size limit")
        with path.open("rb") as stream:
            data = stream.read(maximum + 1)
        after = path.stat()
        if len(data) != before.st_size or (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
            raise ContainerError("resource body changed during read")
        rows = iter(_json_lines(io.BytesIO(data), min(maximum, 64 * 1024 * 1024)))
        header = next(rows, None)
        if not header or not isinstance(header.get("kind"), str) or not isinstance(header.get("object"), str):
            raise ContainerError("resource body has no valid identity header")
        count, unresolved = 0, _unresolved_count(header)
        if header["kind"] in {"DataTable", "StringTable"}:
            for row in rows:
                self.checkpoint()
                valid = (isinstance(row.get("row"), str) and isinstance(row.get("values"), dict)) if header["kind"] == "DataTable" else (
                    isinstance(row.get("key"), str) and isinstance(row.get("source_candidates"), list))
                if not valid:
                    raise ContainerError("invalid table body row")
                count += 1
                unresolved += _unresolved_count(row)
        else:
            if not isinstance(header.get("values"), dict) or next(rows, None) is not None:
                raise ContainerError("invalid object body")
            count = 1
        return data, header, count, unresolved

    def emit(self, record: dict | None, relative: str):
        self.checkpoint()
        data, header, count, unresolved = self.payload(relative)
        orphan = record is None
        if orphan:
            record = {"object": header["object"], "class": header.get("class", header["kind"]),
                      "kind": header["kind"], "categories": ["other_assets"],
                      "records": count, "unresolved": unresolved}
        if record.get("object") != header["object"] or record.get("kind") != header["kind"]:
            raise ContainerError("resource index identity differs from body")
        reported_unresolved = record.get("unresolved", 0)
        if not isinstance(reported_unresolved, int) or reported_unresolved < 0:
            raise ContainerError("invalid resource unresolved count")
        groups = record.get("categories")
        if not isinstance(groups, list) or not groups or any(not isinstance(c, str) or c not in CATEGORIES for c in groups):
            raise ContainerError("invalid resource categories")
        groups = list(dict.fromkeys(groups))
        # The canonical body and every category body are independent byte copies.
        if self.copies + len(groups) + 1 > self.limits.max_files:
            raise ContainerError("resource body file count budget exceeded")
        required = len(data) * (len(groups) + 1)
        if self.payload_bytes + required > self.limits.max_total_mb * 1024 * 1024:
            raise ContainerError("resource body total byte budget exceeded")
        canonical = _body_path(header["object"])
        key = canonical.casefold()
        if key in self.body_paths:
            raise ContainerError(f"resource output path collision: {self.body_paths[key]!r} and {header['object']!r}")
        self.body_paths[key] = header["object"]
        destinations = [canonical, *(c + "/" + canonical for c in groups)]
        for destination in destinations:
            self.checkpoint()
            path = self.output / destination
            if path.resolve() != path:
                raise ContainerError("linked resource output path is not written")
            _write_output(path, data, overwrite=False)
            self.copies += 1
            self.payload_bytes += len(data)
        record = {**record, "file": canonical, "source_file": relative, "categories": groups,
                  "records": count, "source_records": record.get("records"), "bytes": len(data),
                  "sha256": hashlib.sha256(data).hexdigest(), "indexed_in_source": not orphan,
                  "unresolved": max(unresolved, reported_unresolved),
                  "partial": orphan or count != record.get("records") or bool(record.get("partial"))}
        self.line(self.manifest, record)
        for category in groups:
            self.line(self.indexes[category], {**record, "file": category + "/" + canonical})
            self.category_counts[category] += 1
        self.resources += 1
        self.recovered += orphan
        self.partial_resources += record["partial"]
        self.unresolved += record["unresolved"]

    def state(self, status: str, error=None) -> dict:
        result = {"format": "EbonResourceBodies-v1", "status": status,
                  "success": self.resources > 0 and error is None, "source_capture": str(self.capture),
                  "source_capture_status": self.source_state.get("status"),
                  "source_capture_success": self.source_state.get("success"),
                  "source_capture_error": self.source_state.get("error"),
                  "source_unresolved": self.source_state.get("unresolved", 0),
                  "unresolved": self.unresolved,
                  "resources": self.resources, "body_files": self.copies, "recovered_partial_resources": self.recovered,
                  "partial_resources": self.partial_resources,
                  "payload_bytes": self.payload_bytes, "metadata_bytes": self.metadata_bytes,
                  "categories": self.category_counts, "errors": self.errors, "error": error,
                  "file_paths": "relative_to_export_root", "body_naming": "object_path", "category_payloads": "separate_files",
                  "limitations": ["Only existing payload bytes are exported; missing values are not reconstructed.",
                                  "Default objects and unresolved fields retain their original meaning.",
                                  "Unindexed bodies are partial resources, not proof of complete tables."]}
        _write_output(self.output / "resource_bodies_state.json",
                      (json.dumps(result, ensure_ascii=False, indent=2) + "\n").encode("utf-8"), overwrite=self.state_written)
        self.state_written = True
        return result

    def run(self) -> dict:
        self.output.mkdir(parents=True, exist_ok=True)
        with ExitStack() as stack:
            def stream(relative):
                path = self.output / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                return stack.enter_context(path.open("xb"))
            self.manifest = stream("resource_manifest.jsonl")
            self.indexes = {name: stream(name + "/resources.jsonl") for name in CATEGORIES}
            self.state("collecting")
            seen = set()
            try:
                with self.source_path("resource_manifest.jsonl").open("rb") as source:
                    for record in _json_lines(source, 1024 * 1024):
                        self.checkpoint()
                        relative = record.get("file")
                        if not isinstance(relative, str):
                            raise ContainerError("resource index has no file path")
                        relative = _relative_path(relative)
                        if relative in seen:
                            raise ContainerError("duplicate resource body in source manifest")
                        seen.add(relative)
                        self.emit(record, relative)
                # A timed-out table can contain valid rows but never reach index().
                for path in sorted(self.source_path("data").rglob("*.jsonl")):
                    relative = path.relative_to(self.capture).as_posix()
                    if relative not in seen:
                        self.emit(None, relative)
                if self.read_state() != self.source_state:
                    raise ContainerError("source capture state changed during export")
            except (ContainerError, OSError) as error:
                self.errors.append({"reason": str(error)})
                return self.state("failed", str(error))
            partial = self.partial_resources or self.unresolved or self.source_state.get("status") == "complete_with_unresolved" or self.source_state.get("success") is not True or self.source_state.get("unresolved", 0)
            return self.state(("complete_with_unresolved" if partial else "complete") if self.resources else "no_resources")


def export_resource_bodies(capture: Path, output: Path, limits: BundleLimits | None = None) -> dict:
    return ResourceBodyExporter(capture, output, limits).run()
