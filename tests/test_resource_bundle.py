from __future__ import annotations

import ctypes
import hashlib
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch
import zlib

from tests.test_containers import fstring
from tools.ebon.containers import ContainerError
from tools.ebon.oodle import OodleDecoder
from tools.ebon.resource_bundle import BundleLimits, ResourceBundle, dump_resources, file_categories, payload_format
from tools.ebon_cli import main


def make_pak(path: Path, files: dict[str, bytes], *, compression="None"):
    payloads, index_entries = bytearray(), bytearray()
    for name, data in files.items():
        offset = len(payloads)
        encoded = zlib.compress(data) if compression != "None" else data
        header_size = 73 if compression != "None" else 53
        header = struct.pack("<qqqI", offset, len(encoded), len(data), int(compression != "None"))
        header += hashlib.sha1(data).digest()
        if compression != "None":
            header += struct.pack("<iqq", 1, header_size, header_size + len(encoded))
        header += struct.pack("<BI", 0, len(data) if compression != "None" else 0)
        assert len(header) == header_size
        payloads += header + encoded
        index_entries += fstring(name) + header
    index = fstring("../../../HT/Content/") + struct.pack("<i", len(files)) + index_entries
    methods = (compression.encode().ljust(32, b"\0") if compression != "None" else bytes(32)) + bytes(128)
    footer = bytes(17) + struct.pack("<IIqq", 0x5A6F12E1, 8, len(payloads), len(index))
    footer += hashlib.sha1(index).digest() + methods
    path.write_bytes(payloads + index + footer)


def named_utoc(path: Path, data: bytes, name="Maps/Test.umap"):
    invalid = 0xFFFFFFFF
    directory = fstring("../../../HT/Content/") + struct.pack("<i4I", 1, invalid, invalid, invalid, 0)
    directory += struct.pack("<i3I", 1, 0, invalid, 0) + struct.pack("<i", 1) + fstring(name)
    header = b"-==--==--==--==-" + struct.pack("<B3x9I", 8, 144, 1, 1, 12, 0, 32, 65536, len(directory), 1)
    header += bytes(24) + struct.pack("<BBHIQIIII4Q", 8, 0, 0, 0, 0xFFFFFFFFFFFFFFFF, 0, 0, 0, 0, 0, 0, 0, 0)
    chunk = bytes.fromhex("010203040506070800000001")
    path.write_bytes(header + chunk + bytes(5) + len(data).to_bytes(5, "big") +
                     struct.pack("<QI", len(data) << 40, len(data)) + directory + bytes(24))
    path.with_suffix(".ucas").write_bytes(data)


def rows(path: Path):
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]


class ResourceBundleTests(unittest.TestCase):
    def test_loose_lua_binary_lua_hotfix_and_protocol_preserve_exact_bytes(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); client = root / "client"; client.mkdir()
            samples = {"Main.LUA": "return '地图'".encode(), "Module.luac": b"\x1bLua\x54\0\xff",
                       "Data.hpw": b"opaque\0hotfix", "Service.proto": b"syntax = \"proto3\";"}
            for name, data in samples.items():
                (client / name).write_bytes(data)
            output = root / "out"
            state = dump_resources(client, output)
            self.assertTrue(state["success"])
            self.assertEqual(state["extracted"], 4)
            self.assertEqual(state["status"], "complete_with_unresolved")
            self.assertFalse(state["server_resources_complete"])
            for row in rows(output / "resource_files.jsonl"):
                self.assertEqual((output / row["output"]).read_bytes(), samples[row["path"]])
                self.assertEqual(row["sha256"], hashlib.sha256(samples[row["path"]]).hexdigest())
            self.assertEqual(state["categories"]["scripts"]["files"], 2)
            self.assertEqual(state["categories"]["runtime_data"]["files"], 1)

    def test_multiple_paks_do_not_overwrite_patch_variants_or_sidecars(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); client = root / "client"; client.mkdir()
            files = {"Maps/World.umap": b"map", "Maps/World.uexp": b"exports", "Maps/World.ubulk": b"bulk", "Lua/Start.lua": b"return 1"}
            make_pak(client / "base.pak", files)
            make_pak(client / "patch.pak", {"Lua/Start.lua": b"return 2"})
            output = root / "out"
            state = dump_resources(client, output)
            self.assertEqual(state["status"], "complete")
            self.assertEqual(state["extracted"], 5)
            variants = [r for r in rows(output / "resource_files.jsonl") if r["path"].endswith("Start.lua")]
            self.assertEqual(len({r["output"] for r in variants}), 2)
            self.assertEqual({(output / r["output"]).read_bytes() for r in variants}, {b"return 1", b"return 2"})
            self.assertEqual(state["categories"]["maps"]["files"], 3)

    def test_iostore_names_are_retained_but_raw_chunks_are_not_fake_umaps(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); client = root / "client"; client.mkdir()
            named_utoc(client / "test.utoc", b"raw cooked chunk")
            output = root / "out"
            state = dump_resources(client, output)
            self.assertEqual(state["extracted"], 1)
            self.assertEqual(state["status"], "complete_with_unresolved")
            row = rows(output / "resource_files.jsonl")[0]
            self.assertEqual(row["aliases"], ["HT/Content/Maps/Test.umap"])
            self.assertEqual(row["format"], "raw_iostore_chunk")
            self.assertTrue(row["output"].endswith(".bin"))
            self.assertEqual((output / row["output"]).read_bytes(), b"raw cooked chunk")
            self.assertFalse(list(output.rglob("*.umap")))

    def test_injected_decoder_is_used_and_unsupported_compression_does_not_block_other_sources(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); client = root / "client"; client.mkdir()
            make_pak(client / "compressed.pak", {"Lua/Start.lua": b"return 1"}, compression="Oodle")
            (client / "Other.lua").write_bytes(b"return 2")
            state = dump_resources(client, root / "without")
            self.assertEqual((state["extracted"], state["errors"]), (1, 1))
            calls = []
            def decoder(method, data, expected):
                calls.append((method, expected)); return zlib.decompress(data)
            state = dump_resources(client, root / "with", decompressor=decoder)
            self.assertEqual(state["extracted"], 2)
            self.assertEqual(calls, [("Oodle", 8)])

    def test_limits_report_every_blocked_item_and_never_claim_full_export(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); client = root / "client"; client.mkdir()
            for name in ("a.lua", "b.lua", "c.lua"):
                (client / name).write_bytes(b"return 1")
            state = dump_resources(client, root / "out", limits=BundleLimits(max_files=1))
            self.assertEqual((state["indexed"], state["matched"], state["attempted"], state["extracted"], state["blocked"]), (3, 3, 1, 1, 2))
            self.assertEqual(state["status"], "complete_with_unresolved")
            (client / "large.lua").write_bytes(bytes(2 * 1024 * 1024))
            state = dump_resources(client, root / "large", patterns=["large.lua"], limits=BundleLimits(max_file_mb=1))
            self.assertFalse(state["success"])
            self.assertEqual(state["blocked"], 1)
            self.assertEqual(state["attempted"], 0)
            state = dump_resources(client, root / "total", limits=BundleLimits(max_total_mb=1))
            self.assertEqual(state["blocked"], 1)
            self.assertEqual(state["extracted"], 3)

    def test_missing_key_and_malformed_container_are_visible_with_no_false_success(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); client = root / "client"; client.mkdir()
            make_pak(client / "encrypted.pak", {"Lua/Start.lua": b"return 1"})
            pak = client / "encrypted.pak"; data = bytearray(pak.read_bytes()); data[-205] = 1; pak.write_bytes(data)
            (client / "bad.utoc").write_bytes(b"broken")
            state = dump_resources(client, root / "out")
            self.assertFalse(state["success"])
            self.assertEqual(state["unresolved"], 2)
            self.assertTrue(any("verified AES" in r["reason"] for r in rows(root / "out" / "unresolved.jsonl")))

    def test_deadline_and_metadata_budget_write_failed_state(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); client = root / "client"; client.mkdir()
            (client / "a.lua").write_bytes(b"return 1")
            bundle = ResourceBundle(client, root / "deadline"); bundle.deadline = 0
            state = bundle.run()
            self.assertEqual(state["status"], "failed")
            self.assertFalse(state["success"])
            bundle = ResourceBundle(client, root / "metadata", limits=BundleLimits(max_manifest_mb=1))
            bundle.counts["metadata_bytes"] = 1024 * 1024
            state = bundle.run()
            self.assertEqual(state["status"], "failed")
            self.assertIn("manifest byte budget", state["error"])

    def test_separate_new_output_required_and_invalid_paths_cannot_escape(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); client = root / "Neverness To Everness" / "Client" / "Content"; client.mkdir(parents=True)
            for output in (client / "out", client.parents[1] / "out", root):
                with self.assertRaisesRegex(ContainerError, "separate"):
                    dump_resources(client, output)
            (client / "a.lua").write_bytes(b"return 1")
            dump_resources(client, root / "out")
            with self.assertRaisesRegex(ContainerError, "new or empty"):
                dump_resources(client, root / "out")
            make_pak(client / "unsafe.pak", {"../outside.lua": b"return 1"})
            state = dump_resources(client, root / "safe")
            self.assertGreater(state["unresolved"], 0)
            self.assertFalse((root / "outside.lua").exists())

    def test_new_state_never_overwrites_a_file_created_after_output_validation(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); client = root / "client"; client.mkdir()
            bundle = ResourceBundle(client, root / "out")
            bundle.output.mkdir()
            state_path = bundle.output / "resource_bundle_state.json"
            state_path.write_bytes(b"keep existing state")
            with self.assertRaisesRegex(ContainerError, "already exists"):
                bundle.run()
            self.assertEqual(state_path.read_bytes(), b"keep existing state")

    def test_include_is_case_insensitive_and_all_preserves_unknown_extensions(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); client = root / "client"; client.mkdir()
            (client / "Main.LUA").write_bytes(b"return 1"); (client / "Unknown.xyz").write_bytes(b"unknown")
            state = dump_resources(client, root / "selected", patterns=["*.lua"])
            self.assertEqual(state["extracted"], 1)
            self.assertEqual(state["indexed"], 2)
            state = dump_resources(client, root / "all", all_files=True)
            self.assertEqual(state["extracted"], 2)

    def test_capture_validation_and_snapshot_preservation(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); client = root / "client"; client.mkdir()
            capture = root / "res_1"; capture.mkdir()
            (capture / "resource_state.json").write_text(json.dumps({"success": True, "status": "complete_with_unresolved", "unresolved": 1}))
            (capture / "references.jsonl").write_text('{"source":"World","target":"Actor"}\n')
            state = dump_resources(client, root / "out", captures=[capture])
            self.assertEqual(state["captures"], 1)
            self.assertEqual(state["extracted"], 2)
            (capture / "resource_state.json").write_text("[]")
            state = dump_resources(client, root / "bad", captures=[capture])
            self.assertFalse(state["success"])
            self.assertEqual(state["captures"], 0)

    def test_orphan_ucas_and_mutated_loose_files_are_not_silently_accepted(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); client = root / "client"; client.mkdir()
            (client / "orphan_s1.ucas").write_bytes(b"unknown")
            file = client / "a.lua"; file.write_bytes(b"return 1")
            bundle = ResourceBundle(client, root / "out")
            original_emit = bundle.emit
            def mutate(record, size, read, destination, **options):
                if record["path"] == "a.lua":
                    file.write_bytes(b"changed contents")
                return original_emit(record, size, read, destination, **options)
            with patch.object(bundle, "emit", side_effect=mutate):
                state = bundle.run()
            self.assertEqual(state["errors"], 1)
            self.assertEqual(state["unresolved"], 1)
            self.assertFalse(state["success"])
            self.assertIn("changed during read", rows(root / "out" / "resource_files.jsonl")[0]["reason"])

    def test_cli_partial_exit_code_and_no_decoder_execution_on_invalid_hash(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); client = root / "client"; client.mkdir()
            (client / "a.hpw").write_bytes(b"opaque")
            with patch("builtins.print"):
                self.assertEqual(main(["dump-res", "--client", str(client), "--out", str(root / "out")]), 1)
                self.assertEqual(main(["dump-res", "--client", str(client), "--out", str(root / "bad"), "--all", "--include", "*"]), 2)


class OodleAdapterTests(unittest.TestCase):
    def test_abi_arguments_and_exact_output_check_using_mock_export(self):
        decoder = OodleDecoder.__new__(OodleDecoder)
        def decompress(source, size, output, expected, *args):
            self.assertEqual((size, expected), (3, 4))
            self.assertEqual(args, (1, 0, 0, None, 0, None, None, None, 0, 3))
            ctypes.memmove(output, b"test", 4)
            return 4
        decoder.function = decompress
        self.assertEqual(decoder("Oodle", b"abc", 4), b"test")
        decoder.function = lambda *args: -1
        with self.assertRaisesRegex(ContainerError, "mismatch"):
            decoder("Oodle", b"abc", 4)
        with self.assertRaisesRegex(ContainerError, "cannot decode"):
            decoder("Other", b"abc", 4)

    def test_wrong_hash_is_rejected_before_loading_dll(self):
        with tempfile.TemporaryDirectory() as tmp:
            dll = Path(tmp) / "test.dll"; dll.write_bytes(b"not executable")
            with patch("tools.ebon.oodle.os.name", "nt"), patch("tools.ebon.oodle.ctypes.CDLL") as load:
                with self.assertRaisesRegex(ContainerError, "SHA-256 mismatch"):
                    OodleDecoder(dll, "0" * 64)
                load.assert_not_called()

    def test_script_format_is_not_inferred_from_evaluation_substring(self):
        self.assertNotIn("scripts", file_categories("Evaluation.uasset"))
        self.assertEqual(payload_format("a.luac", b"wrapped"), "opaque_script_bytes")
        self.assertEqual(payload_format("a.lua", b"\xff\x01"), "opaque_script_bytes")


if __name__ == "__main__":
    unittest.main()
