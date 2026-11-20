"""Preserve package files referenced by a finished F10 battle capture."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import shutil

from .containers import ContainerError
from .resource_bundle import BundleLimits, ResourceBundle

EXTENSIONS = (".uasset", ".uexp", ".ubulk", ".uptnl", ".m.ubulk", ".umap")


def _package(path: str) -> str | None:
    if not isinstance(path, str) or not path.startswith("/Game/"):
        return None
    package = path.split(".", 1)[0][6:]
    if not package or any(c in package for c in '\\:*?"<>|[]') or any(p in {"", ".", ".."} for p in package.split("/")):
        raise ContainerError("invalid captured battle package path")
    return package


def export_battle_assets(capture: Path, client: Path, output: Path, *, key=None, decompressor=None,
                         limits: BundleLimits | None = None) -> dict:
    capture, client, output = capture.resolve(), client.resolve(), output.resolve()
    if output == capture or output.is_relative_to(capture) or capture.is_relative_to(output):
        raise ContainerError("battle output must be separate from the source capture")
    if output.exists() and (not output.is_dir() or any(output.iterdir())):
        raise ContainerError("battle output must be new or empty")
    state = json.loads((capture / "resource_state.json").read_text(encoding="utf8"))
    if state.get("status") in {"collecting", "discovering"}:
        raise ContainerError("battle capture is still running")
    source = capture / "feature_exports" / "battle_resources.jsonl"
    packages: dict[str, set[str]] = {}
    with source.open(encoding="utf8") as stream:
        for line in stream:
            if len(line) > 64 * 1024 * 1024:
                raise ContainerError("battle reference record exceeds 64 MiB")
            if not line.strip():
                continue
            row = json.loads(line)
            for path in (row.get("object"), row.get("target")):
                package = _package(path)
                if package:
                    packages.setdefault(package, set()).add(path)
            if len(packages) > 65536:
                raise ContainerError("too many battle package references")
    if not packages:
        raise ContainerError("no captured battle package references; take an F10 capture in a loaded scene")
    patterns = [pattern for package in packages for ext in EXTENSIONS for pattern in
                (f"*/Content/{package}{ext}", f"Content/{package}{ext}", f"{package}{ext}")]
    # Constructor validates that the output is outside the installation.
    bundle = ResourceBundle(client, output / "payload", key=key, decompressor=decompressor,
                            patterns=patterns, limits=limits)
    output.mkdir(parents=True, exist_ok=True)
    result = bundle.run()
    preserved = {package: [] for package in packages}
    with (output / "payload" / "resource_files.jsonl").open(encoding="utf8") as stream:
        for line in stream:
            row = json.loads(line)
            if row.get("status") != "extracted":
                continue
            for alias in row.get("aliases", [row["path"]]):
                lower = alias.replace("\\", "/").casefold()
                for package in packages:
                    suffix = package.casefold()
                    if any(lower == suffix + ext or lower == "content/" + suffix + ext or lower.endswith("/content/" + suffix + ext)
                           for ext in EXTENSIONS):
                        if row not in preserved[package]:
                            preserved[package].append(row)
    records = []
    for package, paths in packages.items():
        files = preserved[package]
        raw_only = bool(files) and all(row.get("format") == "raw_iostore_chunk" for row in files)
        has_package = any(row.get("format") != "raw_iostore_chunk" and
                          row["path"].casefold().endswith((".uasset", ".umap")) for row in files)
        records.append({
            "package": "/Game/" + package,
            "load_paths": sorted(paths),
            "status": "package_bytes_preserved" if has_package else
                      "raw_iostore_only" if raw_only else "package_not_exported",
            "files": [{"file": "payload/" + row["output"], "sha256": row["sha256"],
                       "format": row["format"], "source": row["source"]} for row in files],
            "server_load_verified": False,
        })
    shutil.copyfile(source, output / "battle_resources.jsonl")
    report = {
        "format": "EbonBattleAssets-v1",
        "capture": str(capture),
        "build_sha256": state.get("build_sha256"),
        "source_report_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
        "status": "preserved" if all(r["status"] == "package_bytes_preserved" for r in records)
                  and result["status"] == "complete" else "partial",
        "packages": records,
        "bundle_status": result["status"],
        "source_capture_status": state.get("status"),
        "server_load_verified": False,
        "limitations": [
            "Only dependencies observed in captured reflected fields are known.",
            "IoStore raw chunks are not reconstructed Unreal packages.",
            "Native engine/game classes and matching server code are required to load cooked assets.",
            "Preserving package bytes does not prove dedicated-server loading or patch mount priority.",
        ],
    }
    with (output / "battle_assets.json").open("x", encoding="utf8") as stream:
        json.dump(report, stream, ensure_ascii=False, indent=2)
        stream.write("\n")
    return report
