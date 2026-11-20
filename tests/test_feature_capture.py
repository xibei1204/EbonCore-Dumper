import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

from tools.ebon.feature_capture import audit_feature_capture
from tools.ebon.sdk_codegen import CodegenError

ROOT = Path(__file__).resolve().parents[1]
BUILD = Path(os.environ.get("EBON_TEST_BUILD_DIR", ROOT / "build"))
EXE = BUILD / "EbonResourceSelfTest.exe"


@unittest.skipUnless(os.name == "nt" and EXE.is_file(), "build native resource fixtures first")
class FeatureCaptureTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temp.name)
        subprocess.run([str(EXE), str(cls.root)], check=True, capture_output=True, timeout=30)
        cls.capture = cls.root / "features"

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_audit_replays_real_indices_and_preserves_missing_asset_evidence(self):
        result = audit_feature_capture(self.capture)
        self.assertEqual(sorted(result["field_net_indices"].values()), [0, 1, 2])
        self.assertEqual(sorted(c["field_count"] for c in result["cache_chain"]), [0, 0, 3])
        self.assertFalse(result["success"])  # The fixture deliberately has an unloaded table.
        self.assertTrue(any("NotLoaded" in item for item in result["missing"]))
        self.assertFalse((self.capture / "reflection" / "runtime_mapping.json").exists())

    def test_json_index_claim_does_not_override_captured_bytes(self):
        path = self.capture / "reflection" / "network_metadata.json"
        original = path.read_bytes()
        try:
            data = json.loads(original)
            values = data["systematic_game_features_component"]["target_functions"]
            for value in values.values():
                value["observations"][0]["field_net_index"] = 999
            path.write_text(json.dumps(data), encoding="utf8")
            self.assertEqual(sorted(audit_feature_capture(self.capture)["field_net_indices"].values()), [0, 1, 2])
        finally:
            path.write_bytes(original)

    def test_missing_runtime_bytes_never_promote_sdk_declarations(self):
        path = self.capture / "reflection" / "network_memory.bin"
        original = path.read_bytes()
        try:
            path.unlink()
            result = audit_feature_capture(self.capture)
            self.assertTrue(all(value is None for value in result["field_net_indices"].values()))
            self.assertIn("runtime_memory_evidence_unavailable", result["missing"])
        finally:
            path.write_bytes(original)

    def test_mismatched_build_and_body_path_escape_are_rejected(self):
        path = self.capture / "feature_exports" / "systematic_game_features.json"
        original = path.read_bytes()
        try:
            data = json.loads(original)
            data["build_sha256"] = "different"
            path.write_text(json.dumps(data), encoding="utf8")
            with self.assertRaises(CodegenError):
                audit_feature_capture(self.capture)
            data = json.loads(original)
            data["target_assets"][0]["body_file"] = "../escape.jsonl"
            path.write_text(json.dumps(data), encoding="utf8")
            with self.assertRaises(CodegenError):
                audit_feature_capture(self.capture)
        finally:
            path.write_bytes(original)
