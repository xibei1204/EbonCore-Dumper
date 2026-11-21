import contextlib
import io
import json
from pathlib import Path
import tempfile
import unittest

from tools.ebon.gameplay_audit import audit_gameplay_captures
from tools.ebon.sdk_codegen import CodegenError
from tools.ebon_cli import main


def fixture(root, name="capture", sha="synthetic", unresolved=False):
    capture = root / name
    (capture / "feature_exports").mkdir(parents=True)
    (capture / "data").mkdir()
    (capture / "resource_state.json").write_text(json.dumps(dict(status="complete", build_sha256=sha)))
    path = "/Game/Skill/DT_Ability.DT_Ability"
    rows = [dict(kind="DataTable", object=path),
            dict(row="Attack", values=dict(Damage={"_unresolved": "unknown"} if unresolved else 100))]
    (capture / "data" / "ability.jsonl").write_text("".join(json.dumps(row) + "\n" for row in rows))
    report = [dict(object=path, body_file="data/ability.jsonl", records=1, status="captured", unresolved=0),
              dict(kind="asset_reference", target="/Game/Skill/DA_Effect.DA_Effect", target_loaded=False)]
    (capture / "feature_exports" / "battle_resources.jsonl").write_text("".join(json.dumps(row) + "\n" for row in report))
    return capture


class GameplayAuditTests(unittest.TestCase):
    def test_failed_capture_keeps_manifest_bodies_when_battle_report_was_not_written(self):
        with tempfile.TemporaryDirectory() as temporary:
            capture = fixture(Path(temporary))
            report = capture / "feature_exports" / "battle_resources.jsonl"
            body = json.loads(report.read_text().splitlines()[0])
            report.unlink()
            body["file"] = body.pop("body_file")
            (capture / "resource_manifest.jsonl").write_text(json.dumps(body) + "\n")
            (capture / "resource_state.json").write_text(json.dumps(dict(status="failed", build_sha256="synthetic")))
            result = audit_gameplay_captures([capture], include_network=False)
            self.assertEqual(result["summary"]["unique_objects_with_bodies"], 1)
            self.assertEqual(result["captures"][0]["inventory_source"], "resource_manifest.jsonl_fallback")
            self.assertEqual(result["captures"][0]["status"], "failed")

    def test_union_preserves_individual_values_and_detects_hidden_unresolved_markers(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            first, second = fixture(root), fixture(root, "second", unresolved=True)
            before = {p: p.read_bytes() for c in (first, second) for p in c.rglob("*") if p.is_file()}
            result = audit_gameplay_captures([first, second], include_network=False)
            self.assertEqual(result["summary"]["unique_objects_with_bodies"], 1)
            self.assertEqual([b["status"] for b in result["configurations"]], ["captured", "partial"])
            self.assertFalse(result["complete"])
            self.assertEqual(result["missing"][0]["reason"], "unloaded_asset")
            self.assertEqual(before, {p: p.read_bytes() for p in before})

    def test_mixed_builds_and_running_capture_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            first, second = fixture(root), fixture(root, "second", "other-build")
            with self.assertRaisesRegex(CodegenError, "different builds"):
                audit_gameplay_captures([first, second], include_network=False)
            (first / "resource_state.json").write_text(json.dumps(dict(status="collecting", build_sha256="synthetic")))
            with self.assertRaisesRegex(CodegenError, "still running"):
                audit_gameplay_captures([first], include_network=False)

    def test_escaped_and_misidentified_bodies_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            capture = fixture(Path(temporary))
            report = capture / "feature_exports" / "battle_resources.jsonl"
            row = json.loads(report.read_text().splitlines()[0])
            row["body_file"] = "../escape.jsonl"
            report.write_text(json.dumps(row))
            with self.assertRaisesRegex(CodegenError, "escapes"):
                audit_gameplay_captures([capture], include_network=False)
            row["body_file"] = "data/ability.jsonl"
            row["object"] = "/Game/Other.Other"
            report.write_text(json.dumps(row))
            with self.assertRaisesRegex(CodegenError, "identity"):
                audit_gameplay_captures([capture], include_network=False)

    def test_cli_writes_a_new_report_and_refuses_source_mutation(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            capture = fixture(root)
            output = root / "report.json"
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                code = main(["audit-gameplay", "--capture", str(capture), "--skip-network", "--out", str(output)])
                self.assertEqual(code, 1)
                self.assertEqual(main(["audit-gameplay", "--capture", str(capture), "--skip-network",
                                       "--out", str(capture / "bad.json")]), 2)
            self.assertEqual(json.loads(output.read_text())["format"], "EbonGameplayAudit-v1")
            self.assertFalse((capture / "bad.json").exists())
