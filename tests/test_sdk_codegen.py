import json
import tempfile
import unittest
from pathlib import Path

from tools.ebon.sdk_codegen import CodegenError, generate_sdk


def make_dump(root: Path, *, status: str = "complete", field_offset: int = 8) -> Path:
    dump = root / "dump"
    dump.mkdir()
    state = {
        "status": status,
        "types": 2,
        "objects": 1,
        "layout_verified": True,
        "success": True,
        "errors": [],
        "build": {"sha256": "abc"},
    }
    (dump / "dump_state.json").write_text(json.dumps(state), encoding="utf-8")
    types = [
        {
            "kind": "class",
            "path": "/Script/Test.Actor",
            "name": "Actor",
            "size": 16,
            "super": "/Script/CoreUObject.Object",
            "flags": "0x1",
            "fields": [
                {
                    "kind": "property",
                    "name": "Health",
                    "type": "FloatProperty",
                    "offset": field_offset,
                    "element_size": 4,
                    "array_dim": 1,
                    "size": 4,
                    "flags": "0x2",
                }
            ],
        },
        {
            "kind": "enum",
            "path": "/Script/Test.State",
            "name": "State",
            "size": None,
            "fields": [],
            "enum_values": [{"name": "Idle", "value": 0}, {"name": "Run", "value": 1}],
        },
    ]
    objects = [{"index": 0, "path": "/Script/Test.Actor", "name": "Actor"}]
    (dump / "types.jsonl").write_text(
        "".join(json.dumps(row) + "\n" for row in types), encoding="utf-8"
    )
    (dump / "objects.jsonl").write_text(
        "".join(json.dumps(row) + "\n" for row in objects), encoding="utf-8"
    )
    return dump


class SdkCodegenTests(unittest.TestCase):
    def test_generates_reflection_headers_with_layout_metadata(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            dump = make_dump(root)
            output = root / "sdk"
            manifest = generate_sdk(dump, output)
            self.assertEqual(manifest["generated_types"], 2)
            self.assertTrue(manifest["layout_verified"])
            index = (output / "SDK.hpp").read_text(encoding="utf-8")
            self.assertIn("TypeIndexEntry", index)
            type_header = next(
                path for path in (output / "Types").glob("*.hpp")
                if '"Health"' in path.read_text(encoding="utf-8")
            )
            body = type_header.read_text(encoding="utf-8")
            self.assertIn('"Health"', body)
            self.assertIn("8, 4, 1, 4", body)

    def test_rejects_incomplete_runtime_dump(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            dump = make_dump(root, status="incomplete")
            with self.assertRaisesRegex(CodegenError, "does not report a complete"):
                generate_sdk(dump, root / "sdk")

    def test_requires_validated_layout_even_when_status_is_complete(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            dump = make_dump(root)
            state_path = dump / "dump_state.json"
            state = json.loads(state_path.read_text(encoding="utf-8"))
            state["layout_verified"] = False
            state_path.write_text(json.dumps(state), encoding="utf-8")
            with self.assertRaisesRegex(CodegenError, "does not confirm validated"):
                generate_sdk(dump, root / "sdk")

    def test_rejects_field_past_owner_size(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            dump = make_dump(root, field_offset=14)
            manifest = generate_sdk(dump, root / "sdk")
            self.assertEqual(manifest["fields_without_verified_extent"], 1)
            type_header = next(
                path for path in (root / "sdk" / "Types").glob("*.hpp")
                if '"Health"' in path.read_text(encoding="utf-8")
            )
            self.assertIn("14, 4, 1, 4", type_header.read_text(encoding="utf-8"))
            self.assertIn("false", type_header.read_text(encoding="utf-8"))

    def test_rejects_duplicate_owner_paths_and_row_count_drift(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            dump = make_dump(root)
            rows = (dump / "types.jsonl").read_text(encoding="utf-8").splitlines()
            (dump / "types.jsonl").write_text(rows[0] + "\n" + rows[0] + "\n", encoding="utf-8")
            with self.assertRaisesRegex(CodegenError, "duplicate reflected path"):
                generate_sdk(dump, root / "sdk")

    def test_refuses_output_inside_dump(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            dump = make_dump(root)
            with self.assertRaisesRegex(CodegenError, "separate from the source dump"):
                generate_sdk(dump, dump / "sdk")

    def test_refuses_nonempty_sdk_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            dump = make_dump(root)
            output = root / "sdk"
            output.mkdir()
            (output / "keep.txt").write_text("existing", encoding="utf-8")
            with self.assertRaisesRegex(CodegenError, "new or empty"):
                generate_sdk(dump, output)


if __name__ == "__main__":
    unittest.main()
