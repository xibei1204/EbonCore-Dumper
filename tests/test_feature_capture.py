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

    def test_rpc_override_installation_reports_real_binding_and_sample_counts(self):
        failed = json.loads((self.root / "rpc-binding-rejected" / "game_rpc_sample_state.json").read_text(encoding="utf8"))
        self.assertEqual(failed["status"], "no_verified_playerstate_vtable")
        self.assertFalse(failed["installed"])
        binding = failed["binding_diagnostics"][0]
        self.assertTrue(binding["readable"])
        self.assertNotEqual(binding["observed_target"], binding["expected_target"])
        capture = self.root / "rpc-binding"
        state = json.loads((capture / "legacy_sampling_state.json").read_text(encoding="utf8"))
        self.assertTrue(state["installed"])
        self.assertEqual(state["hooked_vtables"], 1)
        self.assertEqual(state["window_samples"], 3)
        self.assertEqual(state["captured_total"], 5)
        self.assertNotEqual(state["root_process_event"], state["playerstate_process_event"])
        samples = [json.loads(line) for line in (capture / "game_rpc_samples.jsonl").read_text(encoding="utf8").splitlines()][:5]
        self.assertEqual([row["message_id"] for row in samples if row["kind"] == "ClientGameDataRPC"], [7, 7, 7, 7])
        self.assertEqual([row["capture_stage"] for row in samples], ["UFunction_native_parameters", "ProcessEvent_parameters", "ProcessEvent_parameters", "UFunction_native_parameters", "UFunction_native_parameters"])
        self.assertEqual(samples[-1]["record_name"], "FogRecord_001")
        self.assertTrue(all(not row["wire_packet_bytes_captured"] for row in samples))

    def test_rpc_sampling_diagnostics_distinguish_invocation_and_decode_rejection(self):
        state = json.loads((self.root / "rpc-binding" / "legacy_sampling_state.json").read_text(encoding="utf8"))
        self.assertEqual(state["status"], "window_expired")
        self.assertEqual(state["native_hooks"], 2)
        self.assertEqual(state["native_binding_status"], "rpc_and_fog_installed")
        self.assertEqual(state["native_calls_total"], 8)
        self.assertEqual(state["window_native_calls"], 5)
        self.assertEqual(state["window_parse_rejected"], 1)
        self.assertEqual(state["window_native_frame_rejected"], 1)
        self.assertEqual(state["window_functions"][0]["function"], "/Script/HTGame.HTPlayerState.ClientGameDataRPC")

    def test_observed_sync_routes_capture_without_legacy_game_data_rpc(self):
        folder = self.root / "rpc-binding"
        state = json.loads((folder / "game_rpc_sample_state.json").read_text(encoding="utf8"))
        self.assertEqual(state["sync_samples_total"], 3)
        self.assertEqual(state["window_sync_calls"], 4)
        self.assertEqual(state["captured_total"], 8)
        self.assertEqual(state["window_parse_rejected"], 1)
        self.assertFalse(state["map_coverage_verified"])
        self.assertTrue(all(target["schema_verified"] for target in state["sync_targets"]))
        samples = [json.loads(line) for line in (folder / "game_rpc_samples.jsonl").read_text(encoding="utf8").splitlines()][5:]
        self.assertEqual([row["kind"] for row in samples], ["ClientDataLayerRPC", "ClientCommonFunRPC", "SendReplicatedObjectPropertyArrayToClient"])
        self.assertEqual([row["message_type"] for row in samples[:2]], [21, 22])
        self.assertEqual(samples[1]["target_actor_address"], "0x12345678")
        properties = samples[2]["containers"][0]["properties"]
        self.assertEqual(properties[0]["captured_payload_hex"], "aabbcc")
        self.assertTrue(properties[0]["compressed"])
        self.assertEqual(properties[0]["name"]["index"], 1234)
        self.assertFalse(samples[2]["compressed_payloads_decoded"])
        self.assertTrue(all(not row["map_coverage_verified"] and not row["wire_packet_bytes_captured"] for row in samples))
        truncated = json.loads((folder / "replicated_truncated.json").read_text(encoding="utf8"))
        self.assertTrue(truncated["truncated"])
        self.assertEqual(truncated["containers"][0]["properties"][0]["captured_payload_hex"], "aabb")

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
