import contextlib
import io
import json
from pathlib import Path
import struct
import tempfile
import unittest

from tools.ebon.sdk_codegen import CodegenError
from tools.verify_friend_rpc_evidence import OWNER, TARGETS, main, verify


SHA = "ab" * 32


def fixture(root):
    reflection, network = root / "reflection", root / "network"
    reflection.mkdir()
    network.mkdir()
    owners = [dict(kind="class", path=OWNER, name="HTPlayerState", address="0x50000",
                   size=40, super=None, fields=[], enum_values=[], complete=True)]
    for index, (path, (_, flag)) in enumerate(TARGETS.items()):
        owners.append(dict(kind="function", path=path, name=path.rsplit(".", 1)[1],
                           address=hex(0x51000 + index * 0x1000), size=32, super=None,
                           flags=hex(flag | 0xC0), fields=[], enum_values=[], complete=True))
    objects = [dict(index=5, address="0x50000", path=OWNER, name="HTPlayerState", **{"class": "Class"}),
               dict(index=6, address="0x70000", path="Transient.Driver", name="Driver", **{"class": "IpNetDriver"})]
    state = dict(status="complete", success=True, layout_verified=True, errors=[],
                 types=len(owners), objects=len(objects), build=dict(sha256=SHA))
    for directory in (reflection, network):
        (directory / "dump_state.json").write_text(json.dumps(state), encoding="utf8")
        (directory / "types.jsonl").write_text("".join(json.dumps(row) + "\n" for row in owners), encoding="utf8")
        (directory / "objects.jsonl").write_text("".join(json.dumps(row) + "\n" for row in objects), encoding="utf8")
    memory = bytearray(0x3000)
    struct.pack_into("<Q", memory, 0, 0x71000)
    struct.pack_into("<ii", memory, 0x1010, 5, 1)
    struct.pack_into("<Qii", memory, 0x1020, 0x72000, 2, 2)
    entries = []
    for index, path in enumerate(TARGETS):
        address = 0x72000 + index * 24
        struct.pack_into("<QII", memory, address - 0x70000, 0x51000 + index * 0x1000, index, 700 + index)
        entries.append(dict(path=path, field_net_index=index, checksum=700 + index, incompatible=False,
                            record_address=hex(address), record_bytes=memory[address - 0x70000:address - 0x70000 + 24].hex()))
    (network / "network_memory.bin").write_bytes(memory)
    (network / "network_memory.json").write_text(json.dumps(dict(format="EbonNetworkMemory-v1",
        blocks=[dict(address="0x70000", offset=0, size=len(memory))])), encoding="utf8")
    cache = dict(kind="class_net_cache", address="0x71000", header_bytes=memory[0x1000:0x1030].hex(),
                 field_stride=24, entries=entries, validation="runtime_cross_checked", **{"class": OWNER})
    mapping = dict(format="EbonRuntimeMapping-v1", capture=str(network.resolve()), module_sha256=SHA,
                   class_net_caches=[cache], rep_layouts=[], packet_verified=True)
    (network / "runtime_mapping.json").write_text(json.dumps(mapping), encoding="utf8")
    metadata = dict(format="EbonNetwork-v1", active_network_objects=[objects[1]])
    (network / "network_metadata.json").write_text(json.dumps(metadata), encoding="utf8")
    return reflection, network


def source_bytes(*directories):
    return {str(path): path.read_bytes() for directory in directories for path in directory.iterdir() if path.is_file()}


class FriendRpcEvidenceTests(unittest.TestCase):
    def test_derives_nonconstant_indices_and_preserves_sources(self):
        with tempfile.TemporaryDirectory() as temporary:
            reflection, network = fixture(Path(temporary))
            before = source_bytes(reflection, network)
            result = verify(reflection, network, SHA)
            self.assertEqual([row["field_net_index"] for row in result["rpcs"]], [0, 1])
            self.assertEqual([row["checksum"] for row in result["rpcs"]], [700, 701])
            self.assertFalse(result["packet_mapping_verified"])
            self.assertFalse(result["wire_packet_bytes_captured"])
            self.assertFalse(result["submessage_encoding_verified"])
            self.assertEqual(result["rpcs"][1]["observations"][0]["record_address"], "0x72018")
            self.assertEqual(source_bytes(reflection, network), before)

    def test_tampered_mapping_or_memory_is_rejected(self):
        for mutation in ("index", "memory", "hash", "parameter", "active"):
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as temporary:
                reflection, network = fixture(Path(temporary))
                if mutation == "index":
                    path = network / "runtime_mapping.json"
                    data = json.loads(path.read_text()); data["class_net_caches"][0]["entries"][0]["field_net_index"] = 999
                    path.write_text(json.dumps(data))
                elif mutation == "memory":
                    path = network / "network_memory.bin"
                    data = bytearray(path.read_bytes()); data[0x2008] = 55; path.write_bytes(data)
                elif mutation == "hash":
                    path = network / "dump_state.json"
                    data = json.loads(path.read_text()); data["build"]["sha256"] = "cd" * 32; path.write_text(json.dumps(data))
                elif mutation == "parameter":
                    path = network / "types.jsonl"
                    rows = [json.loads(line) for line in path.read_text().splitlines()]
                    rows[1]["fields"] = [dict(name="Injected", type="IntProperty", offset=0)]
                    path.write_text("".join(json.dumps(row) + "\n" for row in rows))
                else:
                    path = network / "network_metadata.json"
                    data = json.loads(path.read_text()); data["active_network_objects"][0]["path"] = "Other.Driver"; path.write_text(json.dumps(data))
                before = source_bytes(reflection, network)
                with self.assertRaises(CodegenError):
                    verify(reflection, network, SHA)
                self.assertEqual(source_bytes(reflection, network), before)

    def test_failed_reflection_is_rejected_but_network_errors_are_reported(self):
        with tempfile.TemporaryDirectory() as temporary:
            reflection, network = fixture(Path(temporary))
            path = network / "dump_state.json"
            data = json.loads(path.read_text()); data.update(success=False, errors=["unrelated unloading class"])
            path.write_text(json.dumps(data))
            result = verify(reflection, network, SHA)
            self.assertFalse(result["network_capture_success"])
            self.assertEqual(result["network_capture_errors"], 1)
            path = reflection / "dump_state.json"
            data = json.loads(path.read_text()); data["success"] = False; path.write_text(json.dumps(data))
            with self.assertRaises(CodegenError):
                verify(reflection, network, SHA)

    def test_native_records_fallback_does_not_save_runtime_mapping(self):
        with tempfile.TemporaryDirectory() as temporary:
            reflection, network = fixture(Path(temporary))
            mapping = json.loads((network / "runtime_mapping.json").read_text())
            (network / "runtime_mapping.json").unlink()
            path = network / "network_metadata.json"
            data = json.loads(path.read_text()); data.update(runtime_scan=dict(graph_retained=False),
                runtime_cache_records=mapping["class_net_caches"], rep_layouts=[])
            path.write_text(json.dumps(data))
            before = source_bytes(reflection, network)
            self.assertTrue(verify(reflection, network, SHA)["field_indices_verified"])
            self.assertEqual(source_bytes(reflection, network), before)

    def test_metadata_claims_without_cache_bytes_cannot_verify_indices(self):
        with tempfile.TemporaryDirectory() as temporary:
            reflection, network = fixture(Path(temporary))
            (network / "runtime_mapping.json").unlink()
            path = network / "network_metadata.json"
            data = json.loads(path.read_text())
            data.update(runtime_scan=dict(graph_retained=False), runtime_cache_records=[],
                        rpcs=[dict(path=path, field_net_index=999) for path in TARGETS],
                        packet_mapping_verified=True)
            path.write_text(json.dumps(data))
            before = source_bytes(reflection, network)
            with self.assertRaisesRegex(CodegenError, "not observed"):
                verify(reflection, network, SHA)
            self.assertEqual(source_bytes(reflection, network), before)

    def test_cli_refuses_source_output_and_existing_report(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            reflection, network = fixture(root)
            common = ["--reflection", str(reflection), "--network", str(network), "--expected-sha256", SHA]
            before = source_bytes(reflection, network)
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                main(common + ["--out", str(network / "new.json")])
            report = root / "report.json"
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(main(common + ["--out", str(report)]), 0)
            original = report.read_bytes()
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                main(common + ["--out", str(report)])
            self.assertEqual(report.read_bytes(), original)
            self.assertEqual(source_bytes(reflection, network), before)


if __name__ == "__main__":
    unittest.main()
