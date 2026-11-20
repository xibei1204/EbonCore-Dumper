import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from tools.ebon.battle_assets import export_battle_assets
from tools.ebon.containers import ContainerError


class BattleAssetsTests(unittest.TestCase):
    def fixture(self, root):
        capture = root / "capture"
        (capture / "feature_exports").mkdir(parents=True)
        (capture / "resource_state.json").write_text(json.dumps({"status": "complete", "build_sha256": "fixture"}))
        references = [
            {"kind": "asset_reference", "source": "Hero.Blueprint", "target": "/Game/Characters/BP_Hero.BP_Hero_C"},
            {"object": "/Game/Characters/BP_Hero.Default__BP_Hero_C"},
            {"kind": "asset_reference", "source": "Hero.Skill", "target": "/Game/Skills/DA_Skill.DA_Skill"},
        ]
        (capture / "feature_exports" / "battle_resources.jsonl").write_text(
            "".join(json.dumps(row) + "\n" for row in references))
        client = root / "client"
        (client / "Content" / "Characters").mkdir(parents=True)
        (client / "Content" / "Skills").mkdir()
        for name, data in (("Characters/BP_Hero.uasset", b"asset"), ("Characters/BP_Hero.uexp", b"blueprint"),
                           ("Skills/DA_Skill.uasset", b"skill"), ("Skills/Unrelated.uasset", b"unrelated")):
            (client / "Content" / name).write_bytes(data)
        return capture, client

    def test_exports_exact_packages_and_sidecars_with_load_paths_and_hashes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            capture, client = self.fixture(root)
            result = export_battle_assets(capture, client, root / "output")
            self.assertEqual(result["status"], "preserved")
            self.assertFalse(result["server_load_verified"])
            self.assertEqual(len(result["packages"]), 2)
            files = [file for package in result["packages"] for file in package["files"]]
            self.assertEqual(len(files), 3)
            self.assertTrue(any("/Game/Characters/BP_Hero.BP_Hero_C" in p["load_paths"] for p in result["packages"]))
            for file in files:
                payload = (root / "output" / file["file"]).read_bytes()
                self.assertEqual(hashlib.sha256(payload).hexdigest(), file["sha256"])
            self.assertFalse(any("Unrelated" in file["file"] for file in files))

    def test_missing_packages_remain_explicit_and_sources_are_protected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            capture, client = self.fixture(root)
            (client / "Content" / "Skills" / "DA_Skill.uasset").unlink()
            result = export_battle_assets(capture, client, root / "output")
            self.assertEqual(result["status"], "partial")
            self.assertEqual(next(p["status"] for p in result["packages"] if "/Skills/" in p["package"]), "package_not_exported")
            for output in (capture / "output", client / "output", root / "output"):
                with self.assertRaises(ContainerError):
                    export_battle_assets(capture, client, output)

    def test_raw_iostore_chunks_never_become_loadable_package_claims(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            capture, client = self.fixture(root)

            class RawBundle:
                def __init__(self, client, output, **kwargs):
                    self.output = output

                def run(self):
                    self.output.mkdir(parents=True)
                    rows = [{"status": "extracted", "path": path, "format": "raw_iostore_chunk",
                             "source": "test.utoc", "output": f"chunks/{index}.bin", "sha256": "synthetic"}
                            for index, path in enumerate(("Content/Characters/BP_Hero.uasset", "Content/Skills/DA_Skill.uasset"))]
                    (self.output / "resource_files.jsonl").write_text("".join(json.dumps(row) + "\n" for row in rows))
                    return {"status": "complete_with_unresolved"}

            with patch("tools.ebon.battle_assets.ResourceBundle", RawBundle):
                report = export_battle_assets(capture, client, root / "output")
            self.assertEqual(report["status"], "partial")
            self.assertTrue(all(p["status"] == "raw_iostore_only" and not p["server_load_verified"]
                                for p in report["packages"]))
