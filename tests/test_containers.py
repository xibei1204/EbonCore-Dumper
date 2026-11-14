from __future__ import annotations

import hashlib
from pathlib import Path
import struct
import tempfile
import unittest
import zlib

from tools.ebon.containers import ContainerError, PakFile, UtocFile, inspect_container


def fstring(value: str) -> bytes:
    raw = value.encode("utf-8") + b"\0"
    return struct.pack("<i", len(raw)) + raw


def simple_pak(path: Path, *, encrypted: bool = False) -> None:
    payload = b"sample asset"
    entry_header = struct.pack("<qqqI", 0, len(payload), len(payload), 0)
    entry_header += hashlib.sha1(payload).digest() + struct.pack("<BI", 0, 0)
    index_offset = len(entry_header) + len(payload)
    index = fstring("../../../") + struct.pack("<i", 1) + fstring("Content/Test.uasset")
    index += entry_header
    index_hash = hashlib.sha1(index).digest()
    footer = bytes(16) + bytes([int(encrypted)])
    footer += struct.pack("<I", 0x5A6F12E1) + struct.pack("<Iqq", 8, index_offset, len(index))
    footer += index_hash + (b"\0" * 32) * 5
    path.write_bytes(entry_header + payload + index + footer)


def empty_utoc(path: Path, *, flags: int = 0) -> None:
    header = b"-==--==--==--==-" + struct.pack("<B3x", 8)
    header += struct.pack("<9I", 144, 0, 0, 12, 0, 32, 65536, 0, 1)
    header += bytes(8 + 16) + struct.pack("<BBHIQIIII4Q", flags, 0, 0, 0, 0xFFFFFFFFFFFFFFFF, 0, 0, 0, 0, 0, 0, 0, 0)
    assert len(header) == 144
    path.write_bytes(header)


def one_chunk_utoc(path: Path, payload: bytes, compression: str = "None") -> None:
    encoded = zlib.compress(payload) if compression == "Zlib" else payload
    methods = b"" if compression == "None" else compression.encode() + bytes(32 - len(compression))
    header = b"-==--==--==--==-" + struct.pack("<B3x", 8)
    header += struct.pack("<9I", 144, 1, 1, 12, int(compression != "None"), 32, 65536, 0, 1)
    header += bytes(8 + 16) + struct.pack("<BBHIQIIII4Q", 0, 0, 0, 0, 0xFFFFFFFFFFFFFFFF, 0, 0, 0, 0, 0, 0, 0, 0)
    chunk = bytes.fromhex("010203040506070800000001")
    offset_length = bytes(5) + len(payload).to_bytes(5, "big")
    packed = len(encoded) << 40
    block = struct.pack("<QI", packed, len(payload) | (int(compression != "None") << 24))
    meta = bytes(24)
    assert len(header) == 144
    path.write_bytes(header + chunk + offset_length + block + methods + meta)
    path.with_suffix(".ucas").write_bytes(encoded)


class PakReaderTests(unittest.TestCase):
    def test_v8_index_extracts_only_under_separate_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            client = root / "client"
            client.mkdir()
            pak = client / "test.pak"
            simple_pak(pak)
            reader = PakFile(pak)
            entries = reader.entries()
            self.assertEqual(len(entries), 1)
            self.assertEqual(entries[0].path, "Content/Test.uasset")
            output = root / "out"
            destination = reader.extract(entries[0], output)
            self.assertEqual(destination.read_bytes(), b"sample asset")
            self.assertEqual(len(inspect_container(pak)["entries"]), 1)

    def test_encrypted_index_requires_a_verified_key(self):
        with tempfile.TemporaryDirectory() as temporary:
            pak = Path(temporary) / "encrypted.pak"
            simple_pak(pak, encrypted=True)
            reader = PakFile(pak)
            self.assertTrue(reader.info["encrypted_index"])
            with self.assertRaisesRegex(ContainerError, "verified AES"):
                reader.entries()

    def test_paths_and_overwrite_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            client = root / "client"
            client.mkdir()
            pak = client / "test.pak"
            simple_pak(pak)
            reader = PakFile(pak)
            entry = reader.entries()[0]
            output = root / "out"
            reader.extract(entry, output)
            with self.assertRaisesRegex(ContainerError, "already exists"):
                reader.extract(entry, output)
            traversal = type(entry)("../escape", entry.offset, entry.size, entry.uncompressed_size,
                                    entry.compression, entry.encrypted, entry.blocks, entry.block_size,
                                    entry.header_size, entry.deleted, entry.sha1)
            with self.assertRaisesRegex(ContainerError, "unsafe"):
                reader.extract(traversal, root / "other")


class IoStoreTests(unittest.TestCase):
    def test_header_and_chunk_inventory(self):
        with tempfile.TemporaryDirectory() as temporary:
            toc = Path(temporary) / "global.utoc"
            empty_utoc(toc)
            reader = UtocFile(toc)
            self.assertEqual(reader.info["version"], 8)
            self.assertEqual(list(reader.chunks()), [])
            self.assertEqual(reader.directories(), [])

    def test_reads_uncompressed_and_zlib_chunks(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for compression in ("None", "Zlib"):
                toc = root / f"{compression}.utoc"
                one_chunk_utoc(toc, b"cooked resource", compression)
                reader = UtocFile(toc)
                self.assertEqual(reader.read_chunk(0), b"cooked resource")

    def test_real_installed_containers_are_recognized(self):
        content = Path(r"C:\Program Files\Neverness To Everness\Client\WindowsNoEditor\HT\Content")
        if not content.is_dir():
            self.skipTest("installed client not available")
        paks = sorted(content.glob("**/*.pak"))
        utocs = sorted(content.glob("**/*.utoc"))
        self.assertGreaterEqual(len(paks), 1)
        self.assertGreaterEqual(len(utocs), 1)
        for path in paks:
            with self.subTest(path=path.name):
                reader = PakFile(path)
                self.assertEqual(reader.info["version"], 12)
                self.assertTrue(reader.info["encrypted_index"])
                with self.assertRaisesRegex(ContainerError, "verified AES"):
                    reader.entries()
        for path in utocs:
            with self.subTest(path=path.name):
                reader = UtocFile(path)
                chunks = list(reader.chunks())
                self.assertEqual(len(chunks), reader.info["chunk_count"])
                self.assertTrue(reader.info["signed"])
                self.assertEqual(reader.info["encrypted"], bool(reader.info["flags"] & 2))


if __name__ == "__main__":
    unittest.main()
