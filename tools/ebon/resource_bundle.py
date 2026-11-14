"""Read-only resource preservation across loose files, Pak and IoStore.

Each container/capture keeps a separate namespace: patch mount order is not
guessed and a raw IoStore chunk is never published as a reconstructed package.
"""
from __future__ import annotations

from contextlib import ExitStack
from dataclasses import dataclass
import fnmatch
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import time

from .containers import ContainerError, PakFile, UtocFile, _relative_path, _write_output


CATEGORIES = (
    "maps", "scripts", "config_tables", "localization", "items_economy",
    "combat_ai", "story_quests", "protocols", "runtime_data", "other_assets",
)
RESOURCE_EXTENSIONS = {
    ".umap", ".uasset", ".uexp", ".ubulk", ".uptnl", ".ufont", ".ushaderbytecode",
    ".lua", ".luac", ".luajit", ".as", ".js", ".json", ".csv", ".ini", ".cfg",
    ".xml", ".yaml", ".yml", ".toml", ".txt", ".bin", ".dat", ".bytes", ".hpw",
    ".locres", ".locmeta", ".proto", ".pb", ".sproto", ".fbs", ".sql",
}
CONTAINER_EXTENSIONS = {".pak", ".utoc", ".ucas", ".sig"}


def file_categories(path: str) -> list[str]:
    lower = path.casefold()
    parts = set(lower.replace("\\", "/").split("/"))
    ext = PurePosixPath(lower).suffix
    selected = set()
    if ext == ".umap" or parts & {"maps", "map", "levels", "world", "worlds", "__externalactors__", "__externalobjects__"}:
        selected.add("maps")
    if ext in {".lua", ".luac", ".luajit", ".as", ".js"} or parts & {"lua", "scripts", "script", "unlua", "slua"}:
        selected.add("scripts")
    if ext in {".json", ".csv", ".ini", ".cfg", ".xml", ".yaml", ".yml", ".toml"} or parts & {"config", "datatable", "datatables", "data"}:
        selected.add("config_tables")
    if ext in {".locres", ".locmeta"} or parts & {"localization", "localisation", "text", "stringtable"}:
        selected.add("localization")
    for category, words in {
        "items_economy": ("item", "inventory", "reward", "loot", "drop", "gacha", "lottery", "shop"),
        "combat_ai": ("monster", "combat", "battle", "behaviortree", "blackboard", "skill", "ability"),
        "story_quests": ("quest", "story", "dialog", "mission"),
    }.items():
        if any(word in lower for word in words):
            selected.add(category)
    if ext in {".proto", ".pb", ".sproto", ".fbs"} or parts & {"protocol", "protocols", "network"}:
        selected.add("protocols")
    if ext == ".hpw" or parts & {"runtimedata", "hotfix", "hotupdate", "patch", "patches"}:
        selected.add("runtime_data")
    return [name for name in CATEGORIES if name in selected] or ["other_assets"]


def payload_format(path: str, data: bytes) -> str:
    if data.startswith(b"\x1bLua"):
        return "lua_bytecode"  # Preserved, not decompiled or executed.
    if data.startswith(b"\x1bLJ"):
        return "luajit_bytecode"
    ext = PurePosixPath(path.casefold()).suffix
    if ext in {".luac", ".luajit"}:
        return "opaque_script_bytes"
    if ext in {".lua", ".as", ".js"}:
        try:
            text = data.decode("utf-8-sig")
        except UnicodeError:
            return "opaque_script_bytes"
        return "source_text_candidate" if "\0" not in text else "opaque_script_bytes"
    if ext == ".hpw":
        return "opaque_hpw"
    if ext in {".umap", ".uasset", ".uexp", ".ubulk", ".uptnl"}:
        return "cooked_asset_unparsed"
    return "original_file_bytes"


@dataclass(frozen=True)
class BundleLimits:
    max_files: int = 100_000
    max_file_mb: int = 512
    max_total_mb: int = 32768
    max_manifest_mb: int = 512
    timeout_seconds: int = 1800

    def validate(self) -> None:
        for name, low, high in (("max_files", 1, 5_000_000), ("max_file_mb", 1, 4096),
                                ("max_total_mb", 1, 1_048_576), ("max_manifest_mb", 1, 4096),
                                ("timeout_seconds", 1, 86400)):
            if not low <= getattr(self, name) <= high:
                raise ContainerError(f"{name} outside supported bounds ({low}..{high})")


class ResourceBundle:
    def __init__(self, client: Path, output: Path, *, key=None, decompressor=None,
                 patterns: list[str] | None = None, all_files: bool = False,
                 captures: list[Path] | None = None, limits: BundleLimits | None = None):
        self.client, self.output = client.resolve(), output.resolve()
        self.key, self.decompressor = key, decompressor
        self.patterns, self.all_files = patterns or [], all_files
        self.captures = [path.resolve() for path in captures or []]
        self.limits = limits or BundleLimits()
        self.limits.validate()
        if not self.client.is_dir():
            raise ContainerError(f"client directory does not exist: {self.client}")
        ancestors = (self.client, *self.client.parents)
        protected = next((p for p in ancestors if p.name.casefold() == "neverness to everness"),
                         next((p for p in ancestors if p.name.casefold() == "windowsnoeditor"), self.client))
        for source in (protected, *self.captures):
            if self.output == source or self.output.is_relative_to(source) or source.is_relative_to(self.output):
                raise ContainerError("output must be separate from the read-only client/capture")
        if self.output.exists() and (not self.output.is_dir() or any(self.output.iterdir())):
            raise ContainerError("resource output must be new or empty")
        self.deadline = time.monotonic() + self.limits.timeout_seconds
        self.counts = dict(indexed=0, matched=0, attempted=0, extracted=0, blocked=0, errors=0,
                           unresolved=0, payload_bytes=0, metadata_bytes=0, containers=0, captures=0)
        self.category_counts = {name: 0 for name in CATEGORIES}
        self.diagnostics: list[dict] = []
        self.state_written = False

    def checkpoint(self) -> None:
        if time.monotonic() >= self.deadline:
            raise ContainerError("resource bundle deadline exceeded")

    def line(self, stream, record: dict) -> None:
        self.checkpoint()
        data = json.dumps(record, ensure_ascii=False, separators=(",", ":")) + "\n"
        size = len(data.encode("utf-8"))
        if self.counts["metadata_bytes"] + size > self.limits.max_manifest_mb * 1024 * 1024:
            raise ContainerError("resource manifest byte budget exceeded")
        stream.write(data)
        stream.flush()
        self.counts["metadata_bytes"] += size

    def issue(self, reason: str, **context) -> None:
        record = {"reason": reason, **context}
        self.line(self.issues, record)
        self.counts["unresolved"] += 1
        if len(self.diagnostics) < 20:
            self.diagnostics.append(record)

    def selected(self, path: str) -> bool:
        lower = path.casefold()
        if self.patterns:
            return any(fnmatch.fnmatchcase(lower, p.casefold()) for p in self.patterns)
        return self.all_files or PurePosixPath(lower).suffix in RESOURCE_EXTENSIONS or bool(
            set(lower.split("/")) & {"config", "lua", "scripts", "script", "runtimedata", "hotfix"})

    @staticmethod
    def source_id(kind: str, relative: str) -> str:
        return kind + "_" + hashlib.sha256(relative.encode("utf-8")).hexdigest()

    def files(self, root: Path):
        def walk_error(error):
            self.issue(str(error), source=str(root), stage="directory_scan")
        for parent, dirs, names in os.walk(root, followlinks=False, onerror=walk_error):
            self.checkpoint()
            for name in list(dirs):
                path = Path(parent) / name
                if path.is_symlink() or getattr(path, "is_junction", lambda: False)() or not path.resolve().is_relative_to(root):
                    dirs.remove(name)
                    self.issue("reparse_directory_not_followed", path=str(path))
            dirs.sort(key=str.casefold)
            for name in sorted(names, key=str.casefold):
                self.checkpoint()
                path = Path(parent) / name
                if path.is_symlink() or not path.resolve().is_relative_to(root):
                    self.issue("linked_file_not_read", path=str(path))
                    continue
                yield path

    def emit(self, record: dict, size: int, read, destination: str, *, force=False, raw_chunk=False) -> None:
        self.counts["indexed"] += 1
        selected = force or any(self.selected(name) for name in record.get("aliases", [record["path"]]))
        record = {**record, "bytes": size, "status": "filtered"}
        if selected:
            self.counts["matched"] += 1
            reason = None
            if size < 0 or size > self.limits.max_file_mb * 1024 * 1024:
                reason = "max_file_size_limit_reached"
            elif self.counts["attempted"] >= self.limits.max_files:
                reason = "max_files_limit_reached"
            elif self.counts["payload_bytes"] + size > self.limits.max_total_mb * 1024 * 1024:
                reason = "max_total_size_limit_reached"
            if reason:
                record.update(status="blocked", reason=reason)
                self.counts["blocked"] += 1
            else:
                self.counts["attempted"] += 1
                self.checkpoint()
                # Reject Windows path aliases before any payload is read.
                dest = (self.output / _relative_path(destination)).resolve()
                if not dest.is_relative_to(self.output):
                    raise ContainerError("resource path escapes output root")
                try:
                    data = read()
                    self.checkpoint()
                    if len(data) != size:
                        raise ContainerError("resource changed or read length differs from index")
                    _write_output(dest, data, overwrite=False)
                except (ContainerError, OSError) as error:
                    record.update(status="error", reason=str(error))
                    self.counts["errors"] += 1
                else:
                    record.update(status="extracted", output=dest.relative_to(self.output).as_posix(),
                                  sha256=hashlib.sha256(data).hexdigest(),
                                  format="raw_iostore_chunk" if raw_chunk else payload_format(record["path"], data))
                    self.counts["payload_bytes"] += size
                    self.counts["extracted"] += 1
                    if record["format"] in {"opaque_hpw", "opaque_script_bytes"}:
                        self.issue("preserved_but_payload_format_unverified", source=record["source"], path=record["path"])
            groups = set()
            for alias in record.get("aliases", [record["path"]]):
                groups.update(file_categories(alias))
            if record.get("format") in {"lua_bytecode", "luajit_bytecode"}:
                groups.add("scripts")
            record["categories"] = [name for name in CATEGORIES if name in groups]
            for name in record["categories"]:
                self.line(self.indexes[name], record)
                if record["status"] == "extracted":
                    self.category_counts[name] += 1
        self.line(self.manifest, record)

    def loose(self, path: Path, root: Path, kind: str, source: str, *, force=False) -> None:
        relative = _relative_path(path.relative_to(root).as_posix())
        before = path.stat()
        def read():
            with path.open("rb") as stream:
                data = stream.read(before.st_size + 1)
            after = path.stat()
            if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
                raise ContainerError("loose resource changed during read")
            return data
        self.emit({"source": source, "source_kind": kind, "path": relative}, before.st_size, read,
                  f"sources/{self.source_id(kind, source)}/files/{relative}", force=force)

    def container(self, path: Path) -> None:
        source = path.relative_to(self.client).as_posix()
        kind = path.suffix.casefold()[1:]
        source_root = f"sources/{self.source_id(kind, source)}"
        before = path.stat()
        try:
            reader = (PakFile if kind == "pak" else UtocFile)(path, key=self.key, decompressor=self.decompressor)
            self.counts["containers"] += 1
            self.line(self.containers, {**reader.info, "source": source, "source_id": source_root})
            if kind == "pak":
                for entry in reader.entries():
                    self.checkpoint()
                    if entry.deleted:
                        self.line(self.manifest, {"source": source, "path": entry.path, "status": "deleted", "source_kind": kind})
                        continue
                    def read(entry=entry):
                        data = reader.read_entry(entry, self.limits.max_file_mb * 1024 * 1024)
                        after = path.stat()
                        if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
                            raise ContainerError("Pak changed during extraction")
                        return data
                    self.emit({"source": source, "source_kind": kind, "path": entry.path,
                               "compression": entry.compression, "encrypted": entry.encrypted},
                              entry.uncompressed_size, read, f"{source_root}/files/{entry.path}")
            else:
                names: dict[int, list[str]] = {}
                try:
                    for item in reader.directories():
                        names.setdefault(item["chunk_index"], []).append(item["path"])
                except ContainerError as error:
                    self.issue(str(error), source=source, stage="directory_index")
                self.issue("raw_chunks_only_package_reconstruction_not_implemented", source=source)
                for chunk in reader.chunks():
                    self.checkpoint()
                    aliases = names.get(chunk["index"], [])
                    name = aliases[0] if aliases else "chunks/" + chunk["chunk_id"] + ".bin"
                    self.emit({"source": source, "source_kind": kind, "path": name,
                               "aliases": aliases or [name], **chunk}, chunk["length"],
                              lambda index=chunk["index"]: reader.read_chunk(index, self.limits.max_file_mb * 1024 * 1024),
                              f"{source_root}/chunks/{chunk['index']}_{chunk['chunk_id']}.bin",
                              force=not self.patterns, raw_chunk=True)
        except (ContainerError, OSError) as error:
            self.issue(str(error), source=source, stage="container")

    def state(self, status: str, error=None) -> dict:
        result = {"format": "EbonResourceBundle-v1", "status": status,
                  "success": self.counts["extracted"] > 0 and error is None,
                  "client": str(self.client), **self.counts, "error": error,
                  "categories": {name: {"files": count, "coverage": "preserved_partial" if count else "not_observed"}
                                 for name, count in self.category_counts.items()},
                  "diagnostic_samples": self.diagnostics, "include_patterns": self.patterns,
                  "server_resources_complete": False,
                  "limitations": ["No server-only logic/database is recovered.",
                                  "IoStore chunks need version-aware cooked package reconstruction.",
                                  "Patch mount priority and asset dependency closure are not established.",
                                  "IoStore signatures and chunk hashes are not validated against a trusted signer.",
                                  "Lua bytecode/opaque hotfix bytes are preserved, not decompiled.",
                                  "Unseen/unloaded World Partition cells are not proven covered."],
                  "decompressor": getattr(self.decompressor, "provenance", None)}
        _write_output(self.output / "resource_bundle_state.json",
                      (json.dumps(result, ensure_ascii=False, indent=2) + "\n").encode("utf-8"), overwrite=self.state_written)
        self.state_written = True
        return result

    def run(self) -> dict:
        self.output.mkdir(parents=True, exist_ok=True)
        with ExitStack() as stack:
            def stream(relative):
                path = self.output / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                return stack.enter_context(path.open("x", encoding="utf-8", newline="\n"))
            self.manifest, self.issues = stream("resource_files.jsonl"), stream("unresolved.jsonl")
            self.containers = stream("resource_containers.jsonl")
            self.indexes = {name: stream(f"{name}/resources.jsonl") for name in CATEGORIES}
            self.state("collecting")
            try:
                # Loose runtime files/scripts first so large cooked containers cannot
                # consume the payload budget before these small original files.
                containers = []
                for path in self.files(self.client):
                    ext = path.suffix.casefold()
                    if ext in {".pak", ".utoc"}:
                        containers.append(path)
                    elif ext == ".ucas":
                        stem = re.sub(r"_s\d+$", "", path.stem, flags=re.IGNORECASE)
                        toc = path.with_name(stem + ".utoc")
                        if not toc.is_file():
                            self.issue("orphan_ucas_requires_matching_utoc", source=path.relative_to(self.client).as_posix())
                    elif ext not in CONTAINER_EXTENSIONS:
                        self.loose(path, self.client, "loose", "client")
                for capture in self.captures:
                    try:
                        if (capture / "resource_state.json").stat().st_size > 1024 * 1024:
                            raise ValueError("capture state exceeds 1 MiB")
                        state = json.loads((capture / "resource_state.json").read_text(encoding="utf-8"))
                    except (OSError, ValueError) as error:
                        self.issue(str(error), source=str(capture), stage="capture_state")
                        continue
                    if not isinstance(state, dict) or state.get("success") is not True or state.get("status") not in {"complete", "complete_with_unresolved"}:
                        self.issue("capture_not_successful", source=str(capture))
                        continue
                    if state.get("unresolved", 0):
                        self.issue("capture_contains_unresolved_values", source=str(capture))
                    self.counts["captures"] += 1
                    for path in self.files(capture):
                        self.loose(path, capture, "capture", str(capture), force=True)
                for path in containers:
                    self.container(path)
                status = "complete_with_unresolved" if any(self.counts[k] for k in ("errors", "unresolved", "blocked")) else "complete"
                if not self.counts["extracted"]:
                    status = "no_resources"
                return self.state(status)
            except (ContainerError, OSError) as error:
                return self.state("failed", str(error))


def dump_resources(client: Path, output: Path, **options) -> dict:
    return ResourceBundle(client, output, **options).run()
