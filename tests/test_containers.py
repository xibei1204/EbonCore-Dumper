from __future__ import annotations

import hashlib
from pathlib import Path
import struct
import tempfile
import unittest
import zlib
import os
import subprocess

from tools.ebon.containers import ContainerError, PakFile, UtocFile, inspect_container

try:
    from Crypto.Cipher import AES
except ImportError:
    AES = None


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


def modern_pak(path: Path, *, nte=True, encrypted=True, compressed=True, deleted=False, physical_size=True):
    key = bytes(range(32))
    payload = b'Bounded Pak extraction validates an independently encoded stream.'*7
    packed = zlib.compress(payload) if compressed else payload
    aligned = packed + bytes(-len(packed)%16) if encrypted else packed
    body = AES.new(key, AES.MODE_ECB).encrypt(aligned) if encrypted else aligned
    header_size = 73 if compressed else 53
    stored_size = len(body) if encrypted and compressed and physical_size else len(packed)
    entry = struct.pack('<qqqI', 0, stored_size, len(payload), int(compressed))
    entry += hashlib.sha1(packed).digest()
    if compressed:
        mask = 0x76D6DA if nte else 0
        entry += struct.pack('<Iqq', 1, header_size^mask, (header_size+len(packed))^mask)
    entry += struct.pack('<BI', int(encrypted), len(payload) if compressed else 0)
    directory = struct.pack('<I', 1)+fstring('Content/')+struct.pack('<I', 2 if deleted else 1)
    directory += fstring('asset.bin')+struct.pack('<i', -1)
    if deleted:
        directory += fstring('removed.bin')+struct.pack('<i', -2147483648)
    directory_hash = hashlib.sha1(directory).digest()
    def index(directory_offset):
        return (fstring('../../../')+struct.pack('<IQIIqq', 2 if deleted else 1, 0, 0, 1,
            directory_offset, (len(directory)+15)&~15)+directory_hash+struct.pack('<II', 0, 1)+entry)
    offset = len(entry)+len(body)
    directory_offset = offset+((len(index(0))+15)&~15)
    plain = index(directory_offset)
    index_hash = hashlib.sha1(plain).digest()
    pad = lambda data: data+bytes(-len(data)%16)
    cipher = AES.new(key,AES.MODE_ECB).encrypt(pad(plain))
    directory_cipher = AES.new(key,AES.MODE_ECB).encrypt(pad(directory))
    footer = bytes(16)+b'\1'+struct.pack('<IIqq',0x5A6F12E1,12,offset+int(nte),len(cipher))+index_hash
    footer += b'Zlib'+bytes(28)+bytes(128)
    path.write_bytes(entry+body+cipher+directory_cipher+footer)
    return key, payload


class PakReaderTests(unittest.TestCase):
    @unittest.skipUnless(AES, 'pycryptodome required')
    def test_nte_offset_block_encoding_padding_and_deletions_extract_correctly(self):
        for nte in (False, True):
            for physical_size in (False, True):
                with self.subTest(nte=nte,physical_size=physical_size), tempfile.TemporaryDirectory() as temporary:
                    root=Path(temporary);pak=root/'test.pak'
                    key, payload=modern_pak(pak,nte=nte,deleted=True,physical_size=physical_size)
                    reader=PakFile(pak,key)
                    entries=reader.entries()
                    self.assertEqual(len(entries),1)
                    self.assertEqual(reader.read_entry(entries[0]),payload)
                    self.assertEqual(reader.info['format'],'nte_v12' if nte else 'standard')
                    self.assertEqual(reader.info['index_offset_adjustment'],-1 if nte else 0)
                    self.assertEqual(reader.info['deleted_entries'],1)
                    self.assertEqual(reader.info['deleted_paths'],['Content/removed.bin'])
                    self.assertTrue(reader.info['directory_complete'])

    @unittest.skipUnless(AES, 'pycryptodome required')
    def test_nte_wrong_key_and_corrupt_index_do_not_select_an_alternative(self):
        for corrupt in (False,True):
            with self.subTest(corrupt=corrupt), tempfile.TemporaryDirectory() as temporary:
                pak=Path(temporary)/'test.pak';key,_=modern_pak(pak)
                if corrupt:
                    data=bytearray(pak.read_bytes());data[-221+41]^=1;pak.write_bytes(data)
                else:
                    key=bytes(reversed(key))
                reader=PakFile(pak,key)
                with self.assertRaisesRegex(ContainerError,'SHA-1 mismatch'):
                    reader.entries()
                self.assertEqual(reader.info['format'],'unverified')

    @unittest.skipUnless(AES, 'pycryptodome required')
    def test_encrypted_padding_cannot_extend_into_the_primary_index(self):
        with tempfile.TemporaryDirectory() as temporary:
            pak=Path(temporary)/'test.pak';key,_=modern_pak(pak)
            reader=PakFile(pak,key);entry=reader.entries()[0]
            altered=type(entry)(**{**entry.to_dict(),'blocks':((reader.info['index_offset']-1,reader.info['index_offset']),),'size':1})
            with self.assertRaisesRegex(ContainerError,'padding exceeds'):
                reader._validate_entry(altered)
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

    def test_encrypted_index_with_copied_padding_extracts_and_rejects_wrong_key(self):
        try:
            from Crypto.Cipher import AES
        except ImportError:
            self.skipTest("pycryptodome is required for encrypted Pak fixtures")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            client = root / "client"
            client.mkdir()
            pak = client / "encrypted.pak"
            simple_pak(pak)
            raw = pak.read_bytes()
            footer = bytearray(raw[-221:])
            offset, size = struct.unpack_from("<qq", footer, 25)
            index = raw[offset:offset+size]
            padding = index[:(-len(index) % 16)]
            self.assertTrue(any(padding))
            key = bytes(range(32))
            cipher = AES.new(key, AES.MODE_ECB).encrypt(index+padding)
            footer[16] = 1
            struct.pack_into("<q", footer, 33, len(cipher))
            pak.write_bytes(raw[:offset]+cipher+footer)
            reader = PakFile(pak, key=key)
            entry = reader.entries()[0]
            self.assertEqual(reader.extract(entry, root / "out").read_bytes(), b"sample asset")
            with self.assertRaisesRegex(ContainerError, "SHA-1 mismatch"):
                PakFile(pak, key=bytes(reversed(key))).entries()

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


class OodleProcessTests(unittest.TestCase):
    @unittest.skipUnless(os.name=='nt', 'Windows native helper')
    def test_native_oodle_helper_decodes_raw_blocks_and_rejects_invalid_streams(self):
        from tools.ebon.oodle import OpenSourceOodleDecoder
        root=Path(__file__).resolve().parents[1]
        helper=Path(os.environ.get('EBON_TEST_BUILD_DIR',root/'build'))/'EbonOodleDecode.exe'
        if not helper.is_file():self.skipTest('build EbonOodleDecode.exe')
        decoder=OpenSourceOodleDecoder(helper)
        payload=b'Oodle raw block preserves binary bytes\0\xff'*19
        self.assertEqual(decoder('Oodle',b'\xcc\x06'+payload,len(payload)),payload)
        for data in (b'',b'broken stream',b'\xcc\x06'+payload+b'extra'):
            with self.subTest(data=data[:8]),self.assertRaises(ContainerError):
                decoder('Oodle',data,len(payload))
        result=subprocess.run([str(helper),'--size','-1'],input=b'',capture_output=True,timeout=5)
        self.assertNotEqual(result.returncode,0)


if __name__ == "__main__":
    unittest.main()
