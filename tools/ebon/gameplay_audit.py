"""Audit observed configuration bodies and replication layouts across captures."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path

from .network_coverage import build_coverage_report
from .sdk_codegen import CodegenError

MAX_RECORD = 64 * 1024 * 1024


def _json(path: Path):
    if path.stat().st_size > MAX_RECORD:
        raise CodegenError(f"metadata exceeds 64 MiB: {path}")
    return json.loads(path.read_text(encoding="utf-8"))


def _rows(path: Path):
    with path.open(encoding="utf-8") as stream:
        while line := stream.readline(MAX_RECORD + 1):
            if len(line) > MAX_RECORD:
                raise CodegenError(f"JSONL record exceeds 64 MiB: {path}")
            if line.strip():
                yield json.loads(line)


def _markers(value):
    pending = [value]
    count = 0
    while pending:
        item = pending.pop()
        if isinstance(item, dict):
            count += "_unresolved" in item
            pending.extend(item.values())
        elif isinstance(item, list):
            pending.extend(item)
    return count


def _body(capture, record):
    relative = record["body_file"]
    if not isinstance(relative, str) or Path(relative).is_absolute():
        raise CodegenError("battle body must use a relative path")
    path = (capture / relative).resolve()
    if not path.is_relative_to(capture):
        raise CodegenError("battle body path escapes the capture")
    if not path.is_file():
        return dict(object=record["object"], capture=str(capture), file=relative, status="missing_body")
    records = _rows(path)
    header = next(records, None)
    if not header or header.get("object") != record["object"]:
        raise CodegenError("battle body identity differs from its index")
    table = header.get("kind") == "DataTable"
    observed = 0 if table else 1
    unresolved = _markers(header)
    empty = not bool(header.get("values"))
    row_names = set()
    for row in records:
        if not table or not isinstance(row.get("row"), str) or row["row"] in row_names:
            raise CodegenError("invalid or duplicate battle table row")
        row_names.add(row["row"])
        observed += 1
        unresolved += _markers(row)
        empty = empty and not bool(row.get("values"))
    issues = record.get("unresolved")
    partial = (record.get("status") != "captured" or issues != 0 or unresolved > 0
               or observed != record.get("records"))
    with path.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    return dict(object=record["object"], capture=str(capture), file=relative, sha256=digest,
                kind=header.get("kind"), is_default=bool(record.get("is_default") or header.get("is_default")),
                observed_records=observed, indexed_records=record.get("records"),
                indexed_unresolved=issues, unresolved_markers=unresolved,
                status="partial" if partial else "empty_values" if empty else "captured")


def audit_gameplay_captures(captures: list[Path], *, include_network=True) -> dict:
    if not captures or len(captures) > 64:
        raise CodegenError("audit requires 1..64 F10 capture directories")
    sources = list(dict.fromkeys(Path(c).resolve() for c in captures))
    builds = set()
    states = []
    for capture in sources:
        state = _json(capture / "resource_state.json")
        if state.get("status") in {"initializing", "discovering", "collecting", "writing"}:
            raise CodegenError(f"gameplay capture is still running: {capture}")
        sha = state.get("build_sha256")
        if not isinstance(sha, str) or not sha:
            raise CodegenError("gameplay audit requires a recorded build identity")
        builds.add(sha)
        states.append(state)
    if len(builds) != 1:
        raise CodegenError("cannot combine gameplay captures from different builds")

    configurations = []
    reports = []
    missing = []
    for capture, state in zip(sources, states):
        objects = {}
        references = []
        battle_path = capture / "feature_exports" / "battle_resources.jsonl"
        inventory_source = "battle_resources.jsonl"
        if battle_path.is_file():
            inventory = _rows(battle_path)
        else:
            # Budgets can stop a capture before its final battle report. The
            # manifest still records every completed body; preserve that scope.
            inventory_source = "resource_manifest.jsonl_fallback"
            inventory = (dict(row, body_file=row["file"],
                              status="captured" if row.get("unresolved") == 0 else "partial")
                         for row in _rows(capture / "resource_manifest.jsonl"))
        for row in inventory:
            if row.get("kind") == "asset_reference":
                references.append(row)
            elif isinstance(row.get("object"), str):
                objects[row["object"]] = row
        if not battle_path.is_file() and (capture / "references.jsonl").is_file():
            references.extend(_rows(capture / "references.jsonl"))
        bodies = [_body(capture, row) for row in objects.values() if row.get("body_file")]
        configurations.extend(bodies)
        captured = {b["object"] for b in bodies if b["status"] != "missing_body"}
        defaults = {r.get("class") for r in objects.values() if r.get("is_default") and r["object"] in captured}
        gaps = set()
        for ref in references:
            target = ref.get("target")
            if not isinstance(target, str) or not target.startswith("/Game/"):
                continue
            if target in captured or target in defaults:
                continue
            gaps.add((target, "unloaded_asset" if ref.get("target_loaded") is False else "dependency_body_not_exported"))
        for target, reason in sorted(gaps):
            missing.append(dict(capture=str(capture), target=target, reason=reason))
        priority_path = capture / "feature_exports" / "priority_capture.json"
        pipeline_path = capture / "capture_pipeline.json"
        network = None
        if include_network:
            reflection = capture / "reflection"
            if (reflection / "network_metadata.json").is_file():
                network = build_coverage_report(reflection, include_records=True)
                if network["module_sha256"] != state["build_sha256"]:
                    raise CodegenError("resource and network evidence build identities differ")
            else:
                missing.append(dict(capture=str(capture), reason="network_evidence_not_observed"))
        reports.append(dict(capture=str(capture), status=state.get("status"), inventory_source=inventory_source,
                            candidate_objects=len(objects), body_observations=len(bodies),
                            candidate_bodies_not_exported=len(objects) - len(captured),
                            priority=_json(priority_path) if priority_path.is_file() else None,
                            pipeline=_json(pipeline_path) if pipeline_path.is_file() else None,
                            network=network))
    observed = {b["object"] for b in configurations if b["status"] != "missing_body"}
    clean = {b["object"] for b in configurations if b["status"] == "captured"}
    for gap in missing:
        # An observation in another capture does not change the earlier state.
        gap["body_observed_in_other_capture"] = gap.get("target") in observed
        gap["resolved_values_observed_in_other_capture"] = gap.get("target") in clean
    return dict(format="EbonGameplayAudit-v1", build_sha256=next(iter(builds)),
                status="observed_partial", complete=False,
                summary=dict(captures=len(sources), unique_objects_with_bodies=len({b["object"] for b in configurations}),
                             objects_with_resolved_values=len(clean),
                             partial_or_empty_observations=sum(b["status"] != "captured" for b in configurations),
                             unresolved_asset_references=sum(not g["body_observed_in_other_capture"] for g in missing)),
                captures=reports, configurations=configurations, missing=missing,
                limitations=[
                    "Observed names are candidates; no authoritative list of every character/skill asset is available.",
                    "Bodies and layouts retain their individual capture identity; values and handles are never spliced across sessions.",
                    "Class defaults, empty values and unreflected native/script state do not establish live skill configuration.",
                    "Layout coverage revalidates saved bytes and does not establish packet compatibility.",
                ])
