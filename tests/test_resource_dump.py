"""Validate actual native resource output, including Unicode and sparse holes."""
import json
import os
from pathlib import Path
from tools.ebon.resource_bodies import _body_path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BUILD = Path(os.environ.get("EBON_TEST_BUILD_DIR", ROOT / "build"))
EXE = BUILD / "EbonResourceSelfTest.exe"


@unittest.skipUnless(os.name == "nt" and EXE.is_file(), "build the Windows resource self-test first")
class ResourceDumpTests(unittest.TestCase):
    def test_native_values_are_valid_json_and_preserve_holes_and_text_uncertainty(self):
        with tempfile.TemporaryDirectory(prefix="ebon-resources-") as temporary:
            output = Path(temporary) / "capture"
            result = subprocess.run([str(EXE), str(output)], capture_output=True, timeout=20)
            self.assertEqual(result.returncode, 0, (result.stdout + result.stderr).decode("utf-8", errors="replace"))
            for file in output.rglob("*.json"):
                json.loads(file.read_text(encoding="utf-8"))
            for file in output.rglob("*.jsonl"):
                for line in file.read_text(encoding="utf-8").splitlines():
                    json.loads(line)
            capture = output / "complete"
            state = json.loads((capture / "resource_state.json").read_text(encoding="utf-8"))
            self.assertTrue(state["success"])
            self.assertEqual(state["status"], "complete_with_unresolved")
            rows = [json.loads(line) for line in (capture / "data" / "DataTable" / "DT_CharacterFashion.jsonl").read_text(encoding="utf-8").splitlines()][1:]
            self.assertEqual([row["row"] for row in rows], ["CharacterA", "CharacterB"])
            self.assertEqual(rows[0]["values"]["Title"], "角色一")
            self.assertEqual(rows[0]["values"]["Scores"], [7, 9])
            self.assertEqual(rows[0]["values"]["Lookup"], [{"key": "Level", "value": 10}, {"key": "Rarity", "value": 5}])
            self.assertIn("_unresolved", rows[0]["values"]["UnverifiedText"])
            body_bytes = 0
            for category in state["categories"]:
                index = capture / category / "resources.jsonl"
                for record in map(json.loads, index.read_text(encoding="utf-8").splitlines()):
                    self.assertNotIn("values", record)
                    self.assertTrue(record["file"].startswith(category + "/data/"))
                    body = capture / record["file"]
                    self.assertEqual(body.read_bytes(), (capture / record["source_file"]).read_bytes())
                    body_bytes += body.stat().st_size
            counted_files = [capture / "resource_manifest.jsonl", capture / "unresolved.jsonl", capture / "references.jsonl",
                             capture / "localization" / "text_values.jsonl", *list((capture / "data").rglob("*.jsonl")),
                             *list(capture.glob("*/resources.jsonl")), *list((capture / "feature_exports").iterdir())]
            self.assertEqual(state["bytes"], body_bytes + sum(file.stat().st_size for file in counted_files))
            self.assertEqual(state["category_payloads"], "separate_files")
            features = json.loads((capture / "feature_exports" / "systematic_game_features.json").read_text(encoding="utf-8"))
            self.assertEqual(features["format"], "EbonSystematicGameFeatures-v2")
            self.assertEqual(features["status"], "not_observed")
            self.assertEqual(features["target_assets"], [])
            self.assertEqual(features["component_instances"], [])
            battle_rows = (capture / "feature_exports" / "battle_resources.jsonl").read_text(encoding="utf-8").splitlines()
            self.assertEqual(len(battle_rows), 1)
            self.assertEqual(json.loads(battle_rows[0])["body_file"], "data/DataTable/DT_CharacterFashion.jsonl")
            texts = [json.loads(line) for line in (capture / "localization" / "text_values.jsonl").read_text(encoding="utf-8").splitlines()]
            self.assertTrue(any(text.get("text") == "角色一" for text in texts))
            failed = json.loads((output / "budget" / "resource_state.json").read_text(encoding="utf-8"))
            self.assertFalse(failed["success"])
            self.assertEqual(failed["status"], "failed")
            corrupt = json.loads((output / "corrupt" / "resource_state.json").read_text(encoding="utf-8"))
            self.assertTrue(corrupt["success"])
            self.assertEqual(corrupt["resources"], 1)
            scene = output / "scene"
            component = json.loads((scene / "data" / "Maps" / "MapFixture.PersistentLevel.PlacedNPC.SceneRoot.jsonl").read_text(encoding="utf-8"))
            self.assertEqual(component["values"]["RelativeLocation"], {"X": 1.25, "Y": -2.5, "Z": 3.75})
            self.assertTrue(component["outer"].endswith("PlacedNPC"))
            self.assertTrue(json.loads((scene / "data" / "Script" / "Engine.Default__PlacedNPC.jsonl").read_text(encoding="utf-8"))["is_default"])
            lua = json.loads((scene / "data" / "Lua" / "LuaFixture.jsonl").read_text(encoding="utf-8"))
            self.assertEqual(lua["values"]["Source"], "return { map = '测试地图' }")
            references = [json.loads(line) for line in (scene / "references.jsonl").read_text(encoding="utf-8").splitlines()]
            self.assertTrue(any(ref["source"].endswith("RootComponent") and ref["target"].endswith("SceneRoot") for ref in references))
            self.assertFalse((scene / "data" / "Engine" / "Transient.EvalFixture.jsonl").exists())
            manager = json.loads((scene / "data" / "Engine" / "Transient.QuestRuntime.jsonl").read_text(encoding="utf-8"))
            self.assertEqual(manager["kind"], "GameplayObject")
            self.assertEqual(manager["values"]["CurrentQuestId"], 5150)
            self.assertFalse((output / "scene-focused" / "data" / "Engine" / "Transient.QuestRuntime.jsonl").exists())
            focused = output / "features"
            report = json.loads((focused / "feature_exports" / "systematic_game_features.json").read_text(encoding="utf-8"))
            self.assertEqual(len(report["target_assets"]), 1)
            self.assertFalse(report["target_assets"][0]["is_default"])
            self.assertEqual({t["name"] for t in report["tables"]}, {"DT_SystematicGameFeatures", "FeatureExtra"})
            self.assertTrue(all(t["records"] == 1 and t["status"] == "captured" for t in report["tables"]))
            self.assertTrue(any(r["target"].endswith("NotLoaded.NotLoaded") and not r["target_loaded"] for r in report["references"]))
            component, = report["component_instances"]
            self.assertEqual(component["owner_player"], "/Engine/Transient.Player")
            self.assertEqual(component["outer"], "/Engine/Transient.Player")
            self.assertTrue(component["is_default_subobject"])
            self.assertFalse(component["is_default"])
            def values(record):
                return json.loads((focused / record["body_file"]).read_text(encoding="utf8"))["values"]
            self.assertEqual(values(component)["_super"]["ActivatedFeatureIds"], ["Main"])
            button, = report["menu_buttons"]
            self.assertEqual(values(button), {"GameFeatureId": "Main", "FunctionUnlockId": "Unlock1"})
            feature_rows = [json.loads(l)["record"] for l in
                            (focused / "feature_exports" / "feature_table_rows.jsonl").read_text(encoding="utf8").splitlines()]
            feature_row = next(r["values"] for r in feature_rows if r.get("row") == "Main")
            self.assertEqual(feature_row["Requires"], ["Unlock1"])
            self.assertEqual(feature_row["UINameToOpen"], "MainForm")
            self.assertEqual(feature_row["QuickMenuIndex_Keyboard"], 2)
            self.assertEqual(feature_row["Actions"], [{"_type": "/Script/Test.OpenMenuAction", "value": {"UINameToOpen": "MainForm"}}])
            battle = [json.loads(l) for l in (focused / "feature_exports" / "battle_resources.jsonl").read_text(encoding="utf8").splitlines()]
            self.assertTrue(any(r.get("target") == "/Game/Characters/BP_Hero.BP_Hero_C" for r in battle))
            cdo = next(r for r in battle if r.get("name") == "Default__BP_Hero_C")
            self.assertEqual(values(cdo)["Attack"], 900)
            metadata = json.loads((focused / "network_metadata.json").read_text(encoding="utf8"))
            network = metadata["systematic_game_features_component"]
            self.assertEqual(network["status"], "runtime_observed")
            self.assertEqual(sorted(c["field_count"] for c in network["cache_chain"]), [0, 0, 3])
            names = ["ClientGameFeaturesToReadyForActivate", "ClientOnSystematicGameFeatureActivated", "ServerSetupSystematicGameFeatures"]
            for index, name in enumerate(names):
                self.assertEqual(network["target_functions"][name]["observations"][0]["field_net_index"], index)
            long_path = "/Engine/Transient." + "W" * 290 + ".QuestRuntime"
            body = output / "long-path" / _body_path(long_path)
            self.assertTrue(body.is_file())
            self.assertEqual(json.loads(body.read_text(encoding="utf8"))["object"], long_path)


if __name__ == "__main__":
    unittest.main()
