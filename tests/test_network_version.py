from __future__ import annotations

import copy
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import zlib

from tests.test_cpp_sdk import cpp_fixture
from tools.ebon.cpp_sdk import generate_cpp_sdk
from tools.ebon.network_version import checked_network_version, ue_str_crc32, validate_network_version
from tools.ebon.sdk_codegen import CodegenError
from tools.ebon_cli import main, wait_for_network_version

SHA = "a" * 64
ROOT = Path(__file__).resolve().parents[1]
BUILD = Path(os.environ.get("EBON_TEST_BUILD_DIR", ROOT / "build"))


def evidence(value=0xFFFFFFFF):
    getter, hello, flag = 0x880, 0x980, 0x8000
    prefix = bytearray.fromhex("4883ec78803d0000000000740b8b05000000004883c478c3")
    struct.pack_into("<i", prefix, 6, flag - getter - 11)
    struct.pack_into("<i", prefix, 15, flag + 4 - getter - 19)
    call = b"\xe8" + struct.pack("<i", getter - hello - 5) + bytes.fromhex("89442428")
    cache = b"\x01\0\0\0" + struct.pack("<I", value)
    return dict(format="EbonNetworkVersion-v1", status="observed_cached", verified=True, value=value,
                build_sha256=SHA, module="synthetic-network-version.exe", module_base="0x10000", module_size=0x10000,
                getter_rva=hex(getter), hello_call_rva=hex(hello), cache_flag_rva=hex(flag), cache_value_rva=hex(flag+4),
                getter_bytes=prefix.hex(), hello_call_bytes=call.hex(), cache_bytes=cache.hex(),
                cache_bytes_after=cache.hex(), cache_read_twice=True)


class NetworkVersionTests(unittest.TestCase):
    def test_zero_and_full_uint32_are_real_values_not_unknown_sentinels(self):
        for value in (0, 1, 0x80000000, 0xFFFFFFFF):
            result = validate_network_version(evidence(value), SHA, 0x10000)
            self.assertTrue(result["verified"])
            self.assertEqual(result["value"], value)
            self.assertFalse(result["packet_mapping_verified"])

    def test_tampered_values_bytes_hash_base_and_call_targets_are_rejected(self):
        changes = [dict(value=1), dict(build_sha256="b"*64), dict(module_base="0x20000"),
                   dict(cache_read_twice=False), dict(cache_value_rva="0x8008"),
                   dict(getter_bytes="00"*24), dict(hello_call_bytes="e80000000089442428"),
                   dict(cache_bytes="00"*8), dict(cache_bytes_after="00"*8),
                   dict(module_size=1024), dict(value=True), dict(status="not_initialized")]
        for change in changes:
            with self.subTest(change=change), self.assertRaises(ValueError):
                record=evidence();record.update(change)
                validate_network_version(record,SHA,0x10000)

    def test_uninitialized_cache_never_uses_stale_bytes_or_protocol_version(self):
        record=evidence(123)
        record.update(status="not_initialized", verified=False, value=None, ProtocolVersion=42)
        result=validate_network_version(record,SHA,0x10000)
        self.assertFalse(result["verified"])
        self.assertIsNone(result["value"])

    def test_missing_evidence_is_unknown_and_sdk_exports_nullopt(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);capture=cpp_fixture(root)
            self.assertEqual(checked_network_version(capture)["status"],"not_observed")
            result=generate_cpp_sdk(capture,root/"sdk")
            self.assertIsNone(result["network"]["local_network_version"])
            self.assertIn("LocalNetworkVersion = std::nullopt",(root/"sdk"/"Network.hpp").read_text())

    def test_sdk_consumes_only_same_capture_byte_verified_version(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);capture=cpp_fixture(root)
            state=json.loads((capture/"dump_state.json").read_text())
            state["build"]["sha256"]=SHA;state["module_base"]="0x10000"
            (capture/"dump_state.json").write_text(json.dumps(state))
            path=capture/"network_version.json";path.write_text(json.dumps(evidence(0x12345678)))
            result=generate_cpp_sdk(capture,root/"sdk")
            self.assertEqual(result["network"]["local_network_version"],0x12345678)
            self.assertIn("LocalNetworkVersion = 305419896U",(root/"sdk"/"Network.hpp").read_text())
            raw=evidence();raw["value"]=7;path.write_text(json.dumps(raw))
            with self.assertRaisesRegex(CodegenError,"NetworkVersion"):
                generate_cpp_sdk(capture,root/"bad-sdk")

    def test_waiter_does_not_finish_on_initialization_and_cli_unobserved_exits_nonzero(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);(root/"dump_state.json").write_text(json.dumps({"build":{"sha256":SHA},"module_base":"0x10000"}))
            record=evidence();record.update(status="not_initialized",verified=False,value=None)
            path=root/"network_version.json";path.write_text(json.dumps(record))
            with patch("builtins.print"):
                self.assertEqual(main(["verify-network-version","--capture",str(root)]),1)
                with patch("tools.ebon_cli.time.sleep",side_effect=lambda _:path.write_text(json.dumps(evidence(123)))):
                    self.assertEqual(wait_for_network_version(root,5)["value"],123)
                self.assertEqual(main(["verify-network-version","--capture",str(root)]),0)

    def test_waiter_timeout_remains_unverified(self):
        with tempfile.TemporaryDirectory() as tmp:
            with patch("tools.ebon_cli.time.monotonic",side_effect=[0.0,10.0]):
                with self.assertRaisesRegex(CodegenError,"no guessed value"):
                    wait_for_network_version(Path(tmp),5)

    def test_this_build_crc_expands_utf16_units_to_four_bytes(self):
        self.assertEqual(ue_str_crc32("abc"),zlib.crc32(b"a\0\0\0b\0\0\0c\0\0\0"))
        self.assertNotEqual(ue_str_crc32("abc"),zlib.crc32("abc".encode("utf-16-le")))
        self.assertEqual(ue_str_crc32(""),0)
        with self.assertRaises(ValueError):ue_str_crc32("a\0b")

    @unittest.skipUnless(os.name=="nt" and (BUILD/"EbonReflectionSelfTest.exe").is_file(),"Windows native build required")
    def test_actual_native_observation_json_replays_in_python(self):
        result=subprocess.run([str(BUILD/"EbonReflectionSelfTest.exe"),"--network-version-selftest"],capture_output=True,text=True,timeout=20)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        try:raw=json.loads(result.stdout)
        except json.JSONDecodeError:self.fail("self-test build predates NetworkVersion support")
        checked=validate_network_version(raw,SHA,0x10000)
        self.assertEqual(checked["value"],0xFFFFFFFF)


if __name__=="__main__":unittest.main()
