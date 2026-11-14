"""Separate resource bodies, immutable originals, partial recovery and bounds."""
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from tools.ebon.containers import ContainerError
from tools.ebon.resource_bodies import ResourceBodyExporter, export_resource_bodies
from tools.ebon.resource_bundle import BundleLimits
from tools.ebon_cli import main


def rows(path):
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]


class ResourceBodyTests(unittest.TestCase):
    def fixture(self, root, *, success=True, orphan=False):
        capture = root / "res_1"
        (capture / "data").mkdir(parents=True)
        state = {"format": "EbonResources-v1", "status": "complete" if success else "failed", "success": success,
                 "unresolved": 0, "error": None if success else "resource capture deadline exceeded"}
        (capture / "resource_state.json").write_text(json.dumps(state), encoding="utf-8")
        body = {"kind": "DataAsset", "object": "/Script/HTGame.Default__HTLotteryManager", "class": "/Script/HTGame.HTLotteryManager",
                "is_default": True, "values": {"Title": "活动卡池", "Empty": [], "Missing": None, "Zero": 0, "Flag": False,
                                              "Unknown": {"_unresolved": "unverified", "raw_hex": "00"}}}
        (capture / "data" / "asset_1.jsonl").write_text(json.dumps(body, ensure_ascii=False) + "\n", encoding="utf-8")
        index = {"object": body["object"], "class": "HTLotteryManager", "kind": "DataAsset", "file": "data/asset_1.jsonl",
                 "records": 1, "unresolved": 1, "categories": ["gacha", "characters"]}
        (capture / "resource_manifest.jsonl").write_text(json.dumps(index) + "\n", encoding="utf-8")
        if orphan:
            lines = [{"kind": "DataTable", "object": "/Game/DT_Armor", "row_type": "StaticItemData"},
                     {"row": "armor_a", "values": {"MaxAmount": 1}}, {"row": "armor_b", "values": {"MaxAmount": 2}}]
            (capture / "data" / "table_2.jsonl").write_text("\n".join(map(json.dumps, lines)) + "\n", encoding="utf-8")
        return capture

    def test_indexes_stay_metadata_only_and_all_body_bytes_are_independent(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); capture = self.fixture(root); output = root / "out"
            before = {p.relative_to(capture): p.read_bytes() for p in capture.rglob("*") if p.is_file()}
            state = export_resource_bodies(capture, output)
            self.assertTrue(state["success"])
            self.assertEqual(state["resources"], 1)
            self.assertEqual(state["body_files"], 3)
            self.assertEqual(state["status"], "complete_with_unresolved")
            self.assertEqual(state["unresolved"], 1)
            for category in ("gacha", "characters"):
                record = rows(output / category / "resources.jsonl")[0]
                self.assertNotIn("values", record)
                self.assertEqual(record["file"], category + "/data/asset_1.jsonl")
                payload = (output / record["file"]).read_bytes()
                self.assertEqual(payload, before[Path("data/asset_1.jsonl")])
                self.assertEqual(record["sha256"], hashlib.sha256(payload).hexdigest())
                self.assertEqual(record["bytes"], len(payload))
                self.assertEqual(json.loads(payload)["values"]["Title"], "活动卡池")
            (output / "gacha" / "data" / "asset_1.jsonl").write_bytes(b"edited copy")
            self.assertEqual((output / "characters" / "data" / "asset_1.jsonl").read_bytes(), before[Path("data/asset_1.jsonl")])
            self.assertEqual(before, {p.relative_to(capture): p.read_bytes() for p in capture.rglob("*") if p.is_file()})

    def test_failed_capture_recovers_unindexed_valid_rows_as_explicitly_partial(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); capture = self.fixture(root, success=False, orphan=True); output = root / "out"
            state = export_resource_bodies(capture, output)
            self.assertTrue(state["success"])
            self.assertFalse(state["source_capture_success"])
            self.assertEqual(state["source_capture_status"], "failed")
            self.assertEqual(state["status"], "complete_with_unresolved")
            self.assertEqual(state["recovered_partial_resources"], 1)
            record = rows(output / "other_assets" / "resources.jsonl")[0]
            self.assertTrue(record["partial"])
            self.assertFalse(record["indexed_in_source"])
            self.assertEqual(record["records"], 2)
            self.assertEqual((output / record["file"]).read_bytes(), (capture / "data" / "table_2.jsonl").read_bytes())

    def test_record_count_mismatch_cannot_claim_a_complete_table(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); capture = self.fixture(root); output = root / "out"
            record = rows(capture / "resource_manifest.jsonl")[0]; record["records"] = 2
            (capture / "resource_manifest.jsonl").write_text(json.dumps(record) + "\n")
            state = export_resource_bodies(capture, output)
            self.assertEqual(state["status"], "complete_with_unresolved")
            self.assertEqual(state["partial_resources"], 1)

    def test_refuses_input_overlap_nonempty_output_and_active_capture(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); capture = self.fixture(root)
            for destination in (capture, capture / "new", root):
                with self.assertRaises(ContainerError):
                    export_resource_bodies(capture, destination)
            output = root / "existing"; output.mkdir(); (output / "keep.txt").write_text("keep")
            with self.assertRaises(ContainerError):
                export_resource_bodies(capture, output)
            self.assertEqual((output / "keep.txt").read_text(), "keep")
            (capture / "resource_state.json").write_text(json.dumps({"format": "EbonResources-v1", "status": "collecting"}))
            with self.assertRaisesRegex(ContainerError, "still running"):
                export_resource_bodies(capture, root / "active")

    def test_rejects_escaping_paths_invalid_categories_missing_and_malformed_bodies(self):
        for change in ("../outside.jsonl", "bad_category", "missing", "malformed", "wrong_identity", "duplicate"):
            with self.subTest(change=change), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp); capture = self.fixture(root); record = rows(capture / "resource_manifest.jsonl")[0]
                if change.startswith("../"):
                    record["file"] = change
                elif change == "bad_category":
                    record["categories"] = ["../escape"]
                elif change == "missing":
                    record["file"] = "data/missing.jsonl"
                elif change == "malformed":
                    (capture / "data" / "asset_1.jsonl").write_text('{"values":')
                elif change == "wrong_identity":
                    record["object"] = "/Game/Other"
                (capture / "resource_manifest.jsonl").write_text((json.dumps(record) + "\n") * (2 if change == "duplicate" else 1))
                state = export_resource_bodies(capture, root / "out")
                self.assertFalse(state["success"])
                self.assertEqual(state["status"], "failed")
                self.assertFalse((root / "outside.jsonl").exists())

    def test_copy_and_total_size_budgets_include_every_category_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); capture = self.fixture(root)
            state = export_resource_bodies(capture, root / "files", BundleLimits(max_files=2))
            self.assertFalse(state["success"])
            self.assertIn("file count budget", state["error"])
            self.assertEqual(state["body_files"], 0)
            body = json.loads((capture / "data" / "asset_1.jsonl").read_text(encoding="utf-8")); body["values"]["Large"] = "x" * 400000
            (capture / "data" / "asset_1.jsonl").write_text(json.dumps(body), encoding="utf-8")
            state = export_resource_bodies(capture, root / "bytes", BundleLimits(max_total_mb=1))
            self.assertFalse(state["success"])
            self.assertIn("total byte budget", state["error"])
            self.assertEqual(state["payload_bytes"], 0)

    def test_file_and_metadata_limits_and_deadline_fail_explicitly(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); capture = self.fixture(root)
            body = json.loads((capture / "data" / "asset_1.jsonl").read_text(encoding="utf-8")); body["values"]["Large"] = "x" * 1100000
            (capture / "data" / "asset_1.jsonl").write_text(json.dumps(body), encoding="utf-8")
            state = export_resource_bodies(capture, root / "file", BundleLimits(max_file_mb=1))
            self.assertIn("file size limit", state["error"])
            exporter = ResourceBodyExporter(capture, root / "deadline")
            exporter.deadline = 0
            self.assertIn("deadline exceeded", exporter.run()["error"])
            exporter = ResourceBodyExporter(capture, root / "metadata", BundleLimits(max_manifest_mb=1))
            exporter.metadata_bytes = 1024 * 1024
            self.assertIn("metadata byte budget", exporter.run()["error"])

    def test_state_file_created_after_validation_is_never_overwritten(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); capture = self.fixture(root); exporter = ResourceBodyExporter(capture, root / "out")
            exporter.output.mkdir(); path = exporter.output / "resource_bodies_state.json"; path.write_bytes(b"keep")
            with self.assertRaisesRegex(ContainerError, "already exists"):
                exporter.run()
            self.assertEqual(path.read_bytes(), b"keep")

    def test_cli_exit_codes_preserve_partial_status(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); capture = self.fixture(root, success=False)
            with patch("builtins.print"):
                self.assertEqual(main(["export-resource-bodies", "--capture", str(capture), "--out", str(root / "out")]), 1)
                self.assertEqual(main(["export-resource-bodies", "--capture", str(capture), "--out", str(capture)]), 2)
            state = json.loads((capture / "resource_state.json").read_text()); state.update(status="complete", success=True, error=None)
            (capture / "resource_state.json").write_text(json.dumps(state))
            body = json.loads((capture / "data" / "asset_1.jsonl").read_text(encoding="utf-8")); del body["values"]["Unknown"]
            (capture / "data" / "asset_1.jsonl").write_text(json.dumps(body), encoding="utf-8")
            record = rows(capture / "resource_manifest.jsonl")[0]; record["unresolved"] = 0
            (capture / "resource_manifest.jsonl").write_text(json.dumps(record))
            with patch("builtins.print"):
                self.assertEqual(main(["export-resource-bodies", "--capture", str(capture), "--out", str(root / "complete")]), 0)


if __name__ == "__main__":
    unittest.main()
