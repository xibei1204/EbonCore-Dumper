import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BUILD = Path(os.environ.get("EBON_TEST_BUILD_DIR", ROOT / "build"))
EXE = BUILD / "EbonFriendRpcSelfTest.exe"


@unittest.skipUnless(os.name == "nt" and EXE.is_file(), "build the Friend RPC synthetic host first")
class FriendRpcSamplesTests(unittest.TestCase):
    def test_friend_samples_preserve_both_paths_and_cannot_claim_packet_bytes(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            subprocess.run([str(EXE), folder], check=True, capture_output=True, timeout=30)
            rows = [json.loads(line) for line in (root / "capture" / "game_rpc_samples.jsonl").read_text(encoding="utf8").splitlines()]
            self.assertEqual([row["kind"] for row in rows], ["Client_Friend_RPC", "Server_Friend_RPC"] * 2)
            self.assertEqual([row["direction"] for row in rows], ["client", "server"] * 2)
            self.assertEqual([row["capture_stage"] for row in rows], ["ProcessEvent_parameters"] * 2 + ["UFunction_native_parameters"] * 2)
            for row in rows:
                self.assertFalse(row["wire_packet_bytes_captured"])
                self.assertEqual(row["module_sha256"], "fixture")
                self.assertEqual(row["message_type"], 42)
                self.assertEqual(row["message_id"], 42)
                self.assertEqual(row["wrappers"][0]["captured_payload_hex"], "010203")
                self.assertEqual(row["function"], "/Script/HTGame.HTPlayerState." + row["kind"])
            state = json.loads((root / "capture" / "game_rpc_sample_state.json").read_text(encoding="utf8"))
            self.assertTrue(state["friend_client_native_installed"])
            self.assertTrue(state["friend_server_native_installed"])
            self.assertEqual(state["native_hooks"], 2)
            self.assertEqual(state["capture_scope"], "friends")
            self.assertEqual(state["native_binding_status"], "friend_client_and_server_installed")
            captured = json.loads((root / "capture" / "captured_friend_state.json").read_text(encoding="utf8"))
            self.assertEqual([row["window_samples"] for row in captured["friend_counters"]], [2, 2])
            self.assertEqual(captured["captured_total"], 4)
            empty = json.loads((root / "capture" / "empty_friend_state.json").read_text(encoding="utf8"))
            self.assertEqual(empty["friend_diagnosis"], "friend_functions_not_observed")
            self.assertEqual(empty["captured_total"], 0)
            rearmed = json.loads((root / "capture" / "rearmed_friend_state.json").read_text(encoding="utf8"))
            self.assertEqual((rearmed["window_id"], rearmed["arm_source"]), (2, "request_file"))
            self.assertEqual([row["window_calls"] for row in rearmed["friend_counters"]], [0, 0])
            custom = json.loads((root / "capture" / "custom_friend_sample.json").read_text(encoding="utf8"))
            self.assertEqual(custom["wrappers"][0]["custom_type_name"], "HTFriendInfo")
            self.assertEqual(custom["wrappers"][0]["custom_type_name_index"], 16)
            self.assertEqual(custom["wrappers"][0]["captured_payload_hex"], "0102")
            self.assertTrue(custom["payloads_truncated"])
            self.assertEqual(state["window_parse_rejected"], 1)
            self.assertEqual(state["dropped_total"], 1)
            rejected = json.loads((root / "rejected" / "game_rpc_sample_state.json").read_text(encoding="utf8"))
            self.assertFalse(rejected["installed"])
            self.assertFalse(rejected["wire_packet_bytes_captured"])


if __name__ == "__main__":
    unittest.main()
