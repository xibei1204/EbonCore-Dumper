"""Audit a finished F10 feature capture without inventing absent assets or IDs."""
from __future__ import annotations

import json
from pathlib import Path

from .sdk_codegen import CodegenError
from .runtime_network import RuntimeReplay
from .runtime_mapping import checked_runtime_mapping

TARGET = "/Script/HTGame.SystematicGameFeaturesComponent"
FUNCTIONS = (
    "ClientGameFeaturesToReadyForActivate",
    "ClientOnSystematicGameFeatureActivated",
    "ServerSetupSystematicGameFeatures",
)


def _json(path: Path):
    if path.stat().st_size > 64 * 1024 * 1024:
        raise CodegenError(f"feature metadata exceeds 64 MiB: {path}")
    return json.loads(path.read_text(encoding="utf-8"))


def audit_feature_capture(capture: Path) -> dict:
    capture = capture.resolve()
    state = _json(capture / "resource_state.json")
    if state.get("status") in {"discovering", "collecting"}:
        raise CodegenError("feature capture is still running")
    report = _json(capture / "feature_exports" / "systematic_game_features.json")
    if report.get("format") != "EbonSystematicGameFeatures-v2":
        raise CodegenError("capture requires the new F10 feature exporter")
    missing = list(report.get("missing", []))
    bodies = {}

    def body(record):
        relative = record.get("body_file")
        if not relative:
            missing.append("missing_body: " + record["object"])
            return None
        path = (capture / relative).resolve()
        if not path.is_relative_to(capture):
            raise CodegenError("feature body path escapes the capture")
        lines = []
        with path.open(encoding="utf-8") as stream:
            for line in stream:
                if len(line) > 64 * 1024 * 1024:
                    raise CodegenError("feature body row exceeds 64 MiB")
                if line.strip():
                    lines.append(json.loads(line))
        if not lines or lines[0].get("object") != record["object"]:
            raise CodegenError("feature body identity differs from the index")
        bodies[record["object"]] = lines
        if record.get("status") != "captured":
            missing.append("partial_body: " + record["object"])
        return lines

    for group in ("target_assets", "tables", "component_instances", "menu_buttons"):
        for record in report.get(group, []):
            body(record)
    for asset in report.get("target_assets", []):
        if asset.get("is_default") is not False:
            missing.append("default_asset: " + asset["object"])
        if not any(r["root"] == asset["object"] and ".DataTables" in r["source"]
                   for r in report.get("references", [])):
            missing.append("actual_DataTables_references_empty: " + asset["object"])
    for instance in report.get("component_instances", []):
        if instance.get("is_default") is not False or instance.get("is_default_subobject") is None:
            missing.append("instance_flags_unavailable: " + instance["object"])
        if not instance.get("owner_player"):
            missing.append("instance_owner_unavailable: " + instance["object"])
    tables = []
    for record in report.get("tables", []):
        lines = bodies.get(record["object"], [])
        rows = lines[1:]
        if not rows or len(rows) != record.get("records"):
            missing.append("empty_or_partial_table: " + record["object"])
        if "systematicgamefeatures" in record["object"].casefold():
            for row in rows:
                values = row.get("values", {})
                for key in ("ID", "Requires", "Actions", "UINameToOpen"):
                    if key not in values:
                        missing.append("missing_row_field: " + record["object"] + "." + key)
        tables.append({"object": record["object"], "rows": rows})
    for label in ("SystematicGameFeatures", "FunctionUnlock", "MainFormVisibility"):
        if not any(label.casefold() in t["object"].casefold() for t in tables):
            # A table may have a different asset name; property references are
            # stronger evidence for FunctionUnlock/MainFormVisibility bindings.
            if not any(label.casefold() in r["source"].casefold() and r["target"] in bodies
                       for r in report.get("references", [])):
                missing.append("required_table_not_observed: " + label)
    reflection = capture / "reflection"
    if not reflection.is_dir():
        reflection = capture
    metadata = _json(reflection / "network_metadata.json")
    digest = report.get("build_sha256")
    reflected_state = reflection / "dump_state.json"
    if reflected_state.is_file() and _json(reflected_state).get("build", {}).get("sha256") != digest:
        raise CodegenError("feature tables and network capture have different executable hashes")
    runtime = {"caches": [], "field_indices": []}
    required = ("network_memory.bin", "network_memory.json", "types.jsonl", "objects.jsonl", "dump_state.json")
    if all((reflection / name).is_file() for name in required):
        with (reflection / "types.jsonl").open(encoding="utf8") as stream:
            owners = [json.loads(line) for line in stream]
        replay = RuntimeReplay(reflection).scan()
        runtime = checked_runtime_mapping(reflection, owners, digest, metadata.get("active_network_objects", []), raw_mapping=replay)
    else:
        owners = []
        missing.append("runtime_memory_evidence_unavailable")
    classes = {t["path"]: t for t in owners if t["kind"] == "class"}
    hierarchy, current = set(), TARGET
    while current and current not in hierarchy:
        hierarchy.add(current)
        current = classes.get(current, {}).get("super")
    chain = [c for c in runtime["caches"] if c["class"] in hierarchy]
    indices = {}
    for name in FUNCTIONS:
        observations = [r for r in runtime["field_indices"] if r["path"] == TARGET + "." + name]
        values = {r["field_net_index"] for r in observations
                  if type(r.get("field_net_index")) is int and r["field_net_index"] >= 0
                  and not r.get("incompatible", False)}
        indices[name] = next(iter(values)) if len(values) == 1 else None
        if indices[name] is None:
            missing.append("runtime_field_index_not_observed: " + name)
    if not hierarchy.issubset({c["class"] for c in chain}):
        missing.append("runtime_cache_chain_incomplete")
    if state.get("success") is not True:
        missing.append("source_capture_failed_or_empty")
    return {
        "format": "EbonFeatureAudit-v1",
        "capture": str(capture),
        "success": not missing,
        "status": "complete" if not missing else "incomplete",
        "build_sha256": digest,
        "field_net_indices": indices,
        "cache_chain": chain,
        "assets": report.get("target_assets", []),
        "tables": tables,
        "component_instances": report.get("component_instances", []),
        "menu_buttons": report.get("menu_buttons", []),
        "missing": sorted(set(missing)),
        "packet_verified": False,
    }
