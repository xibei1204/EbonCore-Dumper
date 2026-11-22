"""Exercise the built production hunter against independently encrypted Pak fixtures."""
import hashlib
import io
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import time
import unittest
import sys
from contextlib import redirect_stdout
from unittest.mock import patch

from tools.ebon_cli import wait_for_aes
from tools.ebon.sdk_codegen import CodegenError

try:
    from Crypto.Cipher import AES
except ImportError:
    AES = None

ROOT = Path(__file__).resolve().parents[1]
BUILD = Path(os.environ.get("EBON_TEST_BUILD_DIR", ROOT / "build"))
FINDER = BUILD / "EbonAesFinder.exe"
KEY = bytes.fromhex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f")


def encrypted_pak(path, key=KEY, *, size=333, padded_hash=False, version=12, guid=bytes(16),
                  repeated_padding=False, mount="../../../", wide_mount=False, offset_bias=0):
    # Larger/odd-length index proves the old fixed 160-byte validation is gone.
    mount_bytes = (mount + "\0").encode("utf-16-le" if wide_mount else "utf-8")
    mount_length = -len(mount_bytes)//2 if wide_mount else len(mount_bytes)
    plain = struct.pack("<i", mount_length) + mount_bytes
    plain += bytes((i * 37 + 11) % 256 for i in range(size - len(plain)))
    padding_size = -len(plain) % 16
    padding = plain[:padding_size] if repeated_padding else bytes(padding_size)
    padded = plain + padding
    cipher = AES.new(key, AES.MODE_ECB).encrypt(padded)
    digest = hashlib.sha1(padded if padded_hash else plain).digest()
    offset = 79
    footer = guid + b"\1" + struct.pack("<IIqq", 0x5A6F12E1, version, offset+offset_bias, len(cipher))
    footer += digest + bytes(160)
    path.write_bytes(bytes(offset) + cipher + footer)


def encrypted_utoc(path, key=KEY, *, signed=True, invalid_graph=None, guid=bytes(16)):
    def string(value):
        raw = value.encode('utf-8') + b'\0'
        return struct.pack('<i', len(raw)) + raw
    # Independently serialize one root, one file and its name, including the
    # signed-block table that precedes the encrypted directory on real clients.
    directory = string('../../../HT/') + struct.pack('<I4I', 1, 0xffffffff,
        0 if invalid_graph == 'cycle' else 0xffffffff, 0xffffffff, 0)
    directory += struct.pack('<I3I', 1, 0, 0xffffffff, 1 if invalid_graph == 'chunk' else 0)
    directory += struct.pack('<I', 1) + string('资源.uasset')
    if invalid_graph == 'trailing':
        directory += bytes(32)
    cipher = AES.new(key, AES.MODE_ECB).encrypt(directory + bytes(-len(directory) % 16))
    header = b'-==--==--==--==-' + struct.pack('<B3x9I', 8, 144, 1, 1, 12, 0, 32, 65536, len(cipher), 1)
    header += bytes(8) + guid + struct.pack('<BBHIQIIII4Q', 10 | (4 if signed else 0),
        0, 0, 0, 0xffffffffffffffff, 0, 0, 0, 0, 0, 0, 0, 0)
    assert len(header) == 144
    signatures = struct.pack('<i', 32) + bytes(32*2+20) if signed else b''
    path.write_bytes(header + bytes(12+10+12) + signatures + cipher + bytes(24))


@unittest.skipUnless(os.name == "nt" and FINDER.is_file() and AES, "build native AES tools and install pycryptodome")
class NativeAesTests(unittest.TestCase):
    def run_finder(self, root, candidate, *, expected=0, pak=None, extra=()):
        paks = root / "paks"
        paks.mkdir(exist_ok=True)
        if pak is None:
            pak = paks / "test.pak"
            encrypted_pak(pak)
        source = root / "candidate.bin"
        source.write_bytes(candidate)
        output = root / "result"
        result = subprocess.run([str(FINDER), "--pak", str(pak), "--out", str(output),
                                 "--file", str(source), "--seconds", "15", *extra],
                                capture_output=True, text=True, timeout=25)
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        state = json.loads((output / "aes_state.json").read_text())
        self.assertEqual(state["success"], expected == 0)
        if expected == 0:
            self.assertEqual(state["status"], "verified")
            self.assertEqual(state["key"], "0x" + KEY.hex().upper())
            self.assertEqual((output / "aes_key.txt").read_text().strip(), state["key"])
        else:
            self.assertFalse((output / "aes_key.txt").exists())
        return state

    def test_shared_crypto_implementation_matches_nist_and_sha1_vectors(self):
        result = subprocess.run([str(BUILD / "EbonAesSelfTest.exe")], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_atomic_state_update_retries_transient_windows_reader_lock(self):
        with tempfile.TemporaryDirectory() as temp:
            result = subprocess.run([str(BUILD / "EbonAesSelfTest.exe"), "atomic", str(Path(temp) / "state.json")],
                                    capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_raw_key_unaligned_at_last_possible_offset(self):
        with tempfile.TemporaryDirectory() as temp:
            state = self.run_finder(Path(temp), b"\xff" * 19 + KEY)
            self.assertEqual(state["address"], "0x13")
            self.assertEqual(state["encoding"], "raw")

    def test_nte_offset_bias_requires_complete_index_sha1(self):
        for version, corrupt in ((12, False), (12, True), (11, False)):
            with self.subTest(version=version, corrupt=corrupt), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                (root/'paks').mkdir()
                pak = root/'paks'/'test.pak'
                encrypted_pak(pak, version=version, offset_bias=1)
                if corrupt:
                    raw = bytearray(pak.read_bytes());raw[-221+41] ^= 1;pak.write_bytes(raw)
                expected = 1 if version != 12 else (2 if corrupt else 0)
                state = self.run_finder(root, KEY, pak=pak, expected=expected)
                if state['success']:
                    self.assertEqual(state['verification_method'], 'pak_index_sha1')
                    self.assertEqual(state['samples'][0]['serialized_index_offset'], 80)
                    self.assertEqual(state['samples'][0]['index_offset'], 79)
                    self.assertEqual(state['samples'][0]['index_offset_adjustment'], -1)

    def test_instruction_words_are_reconstructed_across_chunk_boundary(self):
        for frame in ('rbp', 'rsp'):
            with self.subTest(frame=frame), tempfile.TemporaryDirectory() as temp:
                # Separate immediates by actual instructions; KEY never exists
                # contiguously in this scan input, including in reverse order.
                code = b''.join((b'\xc7\x45' + bytes([0xd0+i*4]) if frame == 'rbp'
                    else b'\xc7\x44\x24' + bytes([0x20+i*4])) + KEY[i*4:i*4+4] + b'\x48\x8d\x41\x1f'
                    for i in reversed(range(8)))
                candidate = bytes(1024*1024-19) + code + b'\xc3'
                self.assertNotIn(KEY, candidate)
                state = self.run_finder(Path(temp), candidate)
                self.assertEqual(state['encoding'], 'immediate32')
                self.assertEqual(state['verification_method'], 'pak_index_sha1')

    def test_iostore_directory_verification_accepts_complete_signed_and_unsigned_indexes(self):
        for signed in (False, True):
            with self.subTest(signed=signed), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                paks = root / 'paks'
                paks.mkdir()
                toc = paks / 'test.utoc'
                encrypted_utoc(toc, signed=signed)
                state = self.run_finder(root, KEY, pak=toc)
                self.assertEqual(state['verification_method'], 'io_store_directory_structure')
                self.assertEqual(state['samples'][0]['kind'], 'utoc')
                self.assertIsNone(state['samples'][0]['index_sha1'])

    def test_iostore_rejects_wrong_key_cycles_invalid_chunk_and_trailing_data(self):
        for invalid in ('wrong_key', 'cycle', 'chunk', 'trailing'):
            with self.subTest(invalid=invalid), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                paks = root / 'paks'
                paks.mkdir()
                toc = paks / 'test.utoc'
                encrypted_utoc(toc, invalid_graph=invalid)
                self.run_finder(root, bytes(reversed(KEY)) if invalid == 'wrong_key' else KEY, pak=toc, expected=2)

    def test_directory_discovery_keeps_iostore_when_pak_uses_different_encryption(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            paks = root / 'paks'
            paks.mkdir()
            encrypted_pak(paks / 'small.pak', key=os.urandom(32), size=95)
            encrypted_utoc(paks / 'loaded.utoc')
            state = self.run_finder(root, KEY, pak=paks)
            self.assertEqual(len(state['samples']), 2)
            self.assertEqual(state['samples'][state['verified_sample']]['kind'], 'utoc')

    def test_ascii_and_utf16_hex_cross_chunk_boundary(self):
        for encoding in ("ascii", "utf-16-le"):
            with self.subTest(encoding=encoding), tempfile.TemporaryDirectory() as temp:
                offset = 1024 * 1024 - 13
                state = self.run_finder(Path(temp), bytes(offset) + KEY.hex().encode(encoding))
                self.assertEqual(state["address"], hex(offset))
                self.assertEqual(state["encoding"], "hex" if encoding == "ascii" else "utf16_hex")

    def test_raw_key_crosses_chunk_boundary(self):
        with tempfile.TemporaryDirectory() as temp:
            offset = 1024 * 1024 - 9
            state = self.run_finder(Path(temp), bytes(offset) + KEY + bytes(33))
            self.assertEqual(state["address"], hex(offset))

    def test_large_regions_and_page_read_failures_do_not_drop_later_keys(self):
        for mode in ("large", "gap"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                (root / "paks").mkdir()
                pak = root / "paks" / "test.pak"
                encrypted_pak(pak)
                result = subprocess.run([str(BUILD / "EbonAesSelfTest.exe"), mode, str(pak),
                                         str(root / "result")], capture_output=True, text=True, timeout=15)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                state = json.loads((root / "result" / "aes_state.json").read_text())
                self.assertTrue(state["success"])
                if mode == "gap":
                    self.assertGreater(state["read_failures"], 0)

    def test_live_scan_prioritizes_key_storage_and_aligned_coverage_before_bruteforce(self):
        for mode in ("priority", "image_priority", "chunk", "coverage"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                (root / "paks").mkdir()
                pak = root / "paks" / "test.pak"
                encrypted_pak(pak)
                result = subprocess.run([str(BUILD / "EbonAesSelfTest.exe"), mode, str(pak),
                                         str(root / "result")], capture_output=True, text=True, timeout=15)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                state = json.loads((root / "result" / "aes_state.json").read_text())
                self.assertTrue(state["success"])
                self.assertEqual(state["encoding"], "raw")

    def test_module_snapshot_preserves_readable_ranges_and_marks_holes(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "paks").mkdir()
            pak = root / "paks" / "test.pak"
            encrypted_pak(pak)
            output = root / "result"
            result = subprocess.run([str(BUILD / "EbonAesSelfTest.exe"), "snapshot", str(pak),
                                     str(output)], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            metadata = json.loads((output / "module_memory.json").read_text())
            self.assertEqual(metadata["status"], "partial")
            self.assertEqual(metadata["layout"], "virtual_address")
            self.assertEqual(metadata["captured_ranges"], [[0, 4096], [8192, 4096]])
            data = (output / "module_memory.bin").read_bytes()
            self.assertEqual(data, b"A"*4096 + bytes(4096) + b"A"*4096)
            self.assertFalse((output / "aes_key.txt").exists())

    def test_changed_page_protection_is_rechecked_without_consuming_guard_pages(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "paks").mkdir()
            pak = root / "paks" / "test.pak"
            encrypted_pak(pak)
            result = subprocess.run([str(BUILD / "EbonAesSelfTest.exe"), "protection", str(pak),
                                     str(root / "result")], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            state = json.loads((root / "result" / "aes_state.json").read_text())
            self.assertTrue(state["success"])
            self.assertEqual(state["bytes_unread"], 4096)
            self.assertEqual(state["read_failures"], 1)

    def test_aligned_only_does_not_report_unaligned_raw_key(self):
        with tempfile.TemporaryDirectory() as temp:
            state = self.run_finder(Path(temp), b"\xff" * 19 + KEY, expected=2, extra=("--aligned-only",))
            self.assertEqual(state["status"], "exhausted")

    def test_wrong_key_never_creates_success_or_key_file(self):
        with tempfile.TemporaryDirectory() as temp:
            state = self.run_finder(Path(temp), bytes(reversed(KEY)) + KEY.hex().replace("0", "f").encode(), expected=2)
            self.assertEqual(state["status"], "exhausted")

    def test_footer_hash_corruption_is_not_accepted(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "paks").mkdir()
            pak = root / "paks" / "test.pak"
            encrypted_pak(pak)
            raw = bytearray(pak.read_bytes())
            raw[-221 + 41] ^= 1
            pak.write_bytes(raw)
            self.run_finder(root, KEY, pak=pak, expected=2)

    def test_parser_bounds_and_clear_error_state(self):
        for kind in ("truncated", "oversized", "unencrypted"):
            with self.subTest(kind=kind), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                (root / "paks").mkdir()
                pak = root / "paks" / "test.pak"
                encrypted_pak(pak)
                raw = bytearray(pak.read_bytes())
                if kind == "truncated":
                    raw = raw[:32]
                elif kind == "oversized":
                    struct.pack_into("<Q", raw, len(raw) - 221 + 33, 2**63)
                else:
                    raw[-221 + 16] = 0
                pak.write_bytes(raw)
                state = self.run_finder(root, KEY, pak=pak, expected=1)
                self.assertEqual(state["status"], "error")
                self.assertTrue(state["error"])

    def test_full_padded_hash_and_supported_versions(self):
        for version in (8, 10, 11, 12):
            with self.subTest(version=version), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                (root / "paks").mkdir()
                pak = root / "paks" / "test.pak"
                encrypted_pak(pak, size=173, padded_hash=True, version=version)
                self.run_finder(root, b"\xff" * 8 + KEY, pak=pak)

    def test_nonzero_aes_alignment_padding_does_not_reject_the_correct_key(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "paks").mkdir()
            pak = root / "paks" / "test.pak"
            encrypted_pak(pak, repeated_padding=True)
            self.run_finder(root, KEY, pak=pak)

    def test_mount_terminator_filter_accepts_long_utf8_and_utf16_paths(self):
        for wide_mount in (False, True):
            with self.subTest(wide_mount=wide_mount), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                (root / "paks").mkdir()
                pak = root / "paks" / "test.pak"
                encrypted_pak(pak, mount="../../../Content/资源目录/", wide_mount=wide_mount)
                self.run_finder(root, KEY, pak=pak)

    def test_zero_guid_does_not_hide_other_container_keys(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            paks = root / "paks"
            paks.mkdir()
            encrypted_pak(paks / "small.pak", os.urandom(32), size=95)
            encrypted_pak(paks / "loaded.pak", KEY, size=173)
            state = self.run_finder(root, KEY, pak=paks)
            self.assertEqual(len(state["samples"]), 2)
            verified = state["samples"][state["verified_sample"]]
            self.assertTrue(verified["pak"].endswith("loaded.pak"))

    def test_discovery_selects_smallest_index_per_explicit_guid(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            paks = root / "paks"
            paks.mkdir()
            encrypted_pak(paks / "large.pak", size=4096, guid=b"1" * 16)
            encrypted_pak(paks / "small.pak", size=95, guid=b"1" * 16)
            encrypted_pak(paks / "other.pak", size=512)
            (paks / "bad.pak").write_bytes(b"not a pak")
            state = self.run_finder(root, KEY, pak=paks)
            self.assertEqual(len(state["samples"]), 2)
            self.assertTrue(state["samples"][0]["pak"].endswith("small.pak"))
            self.assertEqual(len(state["warnings"]), 1)

    def test_refuses_stale_output_directory(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            self.run_finder(root, KEY)
            before = (root / "result" / "aes_state.json").read_bytes()
            result = subprocess.run([str(FINDER), "--pak", str(root / "paks"), "--out", str(root / "result"),
                                     "--file", str(root / "candidate.bin")], capture_output=True, text=True)
            self.assertEqual(result.returncode, 1)
            self.assertEqual((root / "result" / "aes_state.json").read_bytes(), before)

    def test_read_only_live_scan_retries_until_key_appears(self):
        # The random key is sent through a pipe only after two completed/started
        # sweeps. It is absent from the target's argv, environment and initial memory.
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "paks").mkdir()
            key = os.urandom(32)
            pak = root / "paks" / "test.pak"
            encrypted_pak(pak, key)
            code = ("import ctypes,sys,time; buf=ctypes.create_string_buffer(256); "
                    "print('ready',flush=True); line=sys.stdin.buffer.readline().strip(); "
                    "buf.value=line; print('loaded',flush=True); time.sleep(30)")
            host = subprocess.Popen([sys.executable, "-c", code], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            scan = None
            try:
                self.assertEqual(host.stdout.readline().strip(), b"ready")
                output = root / "result"
                scan = subprocess.Popen([str(BUILD / "EbonAesScan.exe"), "--pid", str(host.pid),
                                         "--pak", str(pak), "--out", str(output), "--seconds", "15",
                                         "--retry-ms", "25", "--aligned-only"],
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                deadline = time.monotonic() + 12
                while time.monotonic() < deadline:
                    try:
                        state = json.loads((output / "aes_state.json").read_text())
                        if state["sweeps"] >= 2:
                            break
                        if state["status"] == "error":
                            self.fail(state["error"])
                    except (FileNotFoundError, PermissionError, json.JSONDecodeError):
                        pass
                    time.sleep(0.02)
                else:
                    self.fail("live scan never reached a retry sweep")
                host.stdin.write(key.hex().encode() + b"\n")
                host.stdin.flush()
                stdout, stderr = scan.communicate(timeout=20)
                self.assertEqual(scan.returncode, 0, stdout + stderr)
                state = json.loads((output / "aes_state.json").read_text())
                self.assertEqual(state["key"], "0x" + key.hex().upper())
                self.assertEqual(state["pid"], host.pid)
                self.assertGreaterEqual(state["sweeps"], 2)
            finally:
                if scan and scan.poll() is None:
                    scan.kill()
                    scan.communicate()
                host.kill()
                host.communicate()

    def test_dll_background_scan_finishes_after_reflection_without_blocking_it(self):
        import shutil
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            dll = root / "EbonDumper.dll"
            shutil.copy2(BUILD / "EbonDumper.dll", dll)
            # This key never enters the fixture host; its AES worker must time out.
            (root / "paks").mkdir()
            encrypted_pak(root / "paks" / "fixture.pak", os.urandom(32))
            result = subprocess.run([str(BUILD / "EbonDllSelfTest.exe"), "aes", str(dll), str(root / "capture")],
                                    capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            state = json.loads((root / "capture" / "aes" / "aes_state.json").read_text())
            self.assertEqual(state["status"], "timed_out")
            self.assertFalse(state["success"])
            self.assertFalse((root / "capture" / "aes" / "aes_key.txt").exists())
            metadata_path = Path(state["module_snapshot"])
            metadata = json.loads(metadata_path.read_text())
            self.assertEqual(metadata["format"], "EbonAesModuleSnapshot-v1")
            self.assertEqual((metadata_path.parent / metadata["data_file"]).stat().st_size, metadata["module_size"])

    def test_dll_hotkey_listener_waits_for_the_request_file(self):
        import shutil
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            dll = root / "EbonDumper.dll"
            shutil.copy2(BUILD / "EbonDumper.dll", dll)
            (root / "paks").mkdir()
            encrypted_pak(root / "paks" / "fixture.pak", os.urandom(32))
            result = subprocess.run([str(BUILD / "EbonDllSelfTest.exe"), "aes_hotkey", str(dll), str(root / "capture")],
                                    capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            listener = json.loads((root / "capture" / "aes_hotkey_state.json").read_text())
            self.assertEqual(listener["format"], "EbonAesHotkey-v1")
            self.assertEqual(listener["key"], "F9")
            self.assertFalse(listener["terminal_available"])
            self.assertEqual(listener["runs"], 1)
            state = json.loads((root / "capture" / "aes" / "aes_state.json").read_text())
            self.assertEqual(state["status"], "timed_out")
            self.assertFalse((root / "capture" / "aes" / "aes_key.txt").exists())

    def test_dll_hotkey_dumps_a_verified_unaligned_heap_key(self):
        import shutil
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            dll = root / "EbonDumper.dll"
            shutil.copy2(BUILD / "EbonDumper.dll", dll)
            (root / "paks").mkdir()
            key = os.urandom(32)
            encrypted_pak(root / "paks" / "fixture.pak", key)
            (root / "fixture-key.bin").write_bytes(key)
            result = subprocess.run([str(BUILD / "EbonDllSelfTest.exe"), "aes_hotkey_verified",
                                     str(dll), str(root / "capture")], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            output = root / "capture" / "aes"
            state = json.loads((output / "aes_state.json").read_text())
            self.assertEqual(state["status"], "verified")
            self.assertTrue(state["success"])
            self.assertEqual(state["encoding"], "raw")
            self.assertEqual(state["source"], "in_process_memory")
            self.assertEqual(state["key"], "0x" + key.hex().upper())
            self.assertEqual((output / "aes_key.txt").read_text().strip(), state["key"])

    def test_dll_constructor_failure_has_terminal_state_and_keeps_reflection_working(self):
        import shutil
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            dll = root / "EbonDumper.dll"
            shutil.copy2(BUILD / "EbonDumper.dll", dll)
            result = subprocess.run([str(BUILD / "EbonDllSelfTest.exe"), "aes_error", str(dll), str(root / "capture")],
                                    capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            state = json.loads((root / "capture" / "aes" / "aes_state.json").read_text())
            self.assertEqual(state["status"], "error")
            self.assertIn("outside the Pak directory", state["error"])

    def test_dll_hotkey_verifies_iostore_and_writes_the_actual_key(self):
        import shutil
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            dll = root / 'EbonDumper.dll'
            shutil.copy2(BUILD / 'EbonDumper.dll', dll)
            (root / 'paks').mkdir()
            key = os.urandom(32)
            encrypted_utoc(root / 'paks' / 'fixture.utoc', key)
            (root / 'fixture-key.bin').write_bytes(key)
            result = subprocess.run([str(BUILD / 'EbonDllSelfTest.exe'), 'aes_hotkey_iostore',
                str(dll), str(root / 'capture')], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            output = root / 'capture' / 'aes'
            state = json.loads((output / 'aes_state.json').read_text())
            self.assertEqual(state['verification_method'], 'io_store_directory_structure')
            self.assertEqual(state['key'], '0x' + key.hex().upper())
            self.assertEqual((output / 'aes_key.txt').read_text().strip(), state['key'])


class AesWaitTests(unittest.TestCase):
    def test_repeat_scan_uses_latest_listener_output_instead_of_stale_first_result(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "aes").mkdir()
            (root / "aes" / "aes_state.json").write_text(json.dumps(
                dict(format="EbonAES-v1", status="timed_out", success=False)))
            latest = root / "aes_2"
            latest.mkdir()
            state_path = latest / "aes_state.json"
            state_path.write_text(json.dumps(dict(format="EbonAES-v1", status="scanning", success=False)))
            (root / "aes_hotkey_state.json").write_text(json.dumps(
                dict(format="EbonAesHotkey-v1", status="running", last_output=str(latest))))
            key = "0x" + KEY.hex().upper()
            def finish(_):
                (latest / "aes_key.txt").write_text(key + "\n")
                state_path.write_text(json.dumps(dict(format="EbonAES-v1", status="verified", success=True, key=key)))
            with patch("tools.ebon_cli.time.sleep", side_effect=finish):
                self.assertTrue(wait_for_aes(root)["success"])

    def test_repeat_scan_waits_for_latest_state_even_before_it_is_published(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "aes").mkdir()
            (root / "aes" / "aes_state.json").write_text(json.dumps(
                dict(format="EbonAES-v1", status="timed_out", success=False)))
            latest = root / "aes_2"
            (root / "aes_hotkey_state.json").write_text(json.dumps(
                dict(format="EbonAesHotkey-v1", status="running", last_output="aes_2")))
            def finish(_):
                latest.mkdir(exist_ok=True)
                (latest / "aes_state.json").write_text(json.dumps(
                    dict(format="EbonAES-v1", status="exhausted", success=False)))
            with patch("tools.ebon_cli.time.sleep", side_effect=finish):
                self.assertEqual(wait_for_aes(root)["status"], "exhausted")

    def test_listener_output_must_belong_to_this_capture(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "aes_hotkey_state.json").write_text(json.dumps(
                dict(format="EbonAesHotkey-v1", status="running", last_output=str(root.parent / "aes_2"))))
            with self.assertRaisesRegex(CodegenError, "invalid AES listener output"):
                wait_for_aes(root)

    def test_waits_for_terminal_state_in_specific_capture(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "aes").mkdir()
            state_path = root / "aes" / "aes_state.json"
            state_path.write_text(json.dumps(dict(format="EbonAES-v1", status="scanning", success=False)))
            def finish(_):
                state_path.write_text(json.dumps(dict(format="EbonAES-v1", status="timed_out", success=False)))
            with patch("tools.ebon_cli.time.sleep", side_effect=finish):
                self.assertEqual(wait_for_aes(root)["status"], "timed_out")

    def test_verified_state_requires_matching_key_file(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "aes").mkdir()
            state_path = root / "aes" / "aes_state.json"
            key = "0x" + KEY.hex().upper()
            state_path.write_text(json.dumps(dict(format="EbonAES-v1", status="verified", success=True, key=key)))
            with self.assertRaisesRegex(CodegenError, "does not match"):
                wait_for_aes(root)
            (root / "aes" / "aes_key.txt").write_text(key + "\n")
            self.assertTrue(wait_for_aes(root)["success"])

    def test_missing_worker_times_out(self):
        with patch("tools.ebon_cli.time.monotonic", side_effect=[0, 181]):
            with self.assertRaisesRegex(CodegenError, "AES did not finish"):
                wait_for_aes(Path("missing"))

    def test_hotkey_armed_capture_reports_the_listener(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "aes_hotkey_state.json").write_text(json.dumps(
                dict(format="EbonAesHotkey-v1", status="armed", key="F9", terminal_available=True)))
            out = io.StringIO()
            with patch("tools.ebon_cli.time.monotonic", side_effect=[0, 0, 181]), \
                 patch("tools.ebon_cli.time.sleep", lambda *_: None), \
                 redirect_stdout(out):
                with self.assertRaisesRegex(CodegenError, "AES did not finish"):
                    wait_for_aes(root, timeout=1)
            self.assertIn("press F9 in the dumper terminal", out.getvalue())

    def test_hotkey_without_terminal_points_at_the_request_file(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "aes_hotkey_state.json").write_text(json.dumps(
                dict(format="EbonAesHotkey-v1", status="armed", key="F9", terminal_available=False)))
            out = io.StringIO()
            with patch("tools.ebon_cli.time.monotonic", side_effect=[0, 0, 181]), \
                 patch("tools.ebon_cli.time.sleep", lambda *_: None), \
                 redirect_stdout(out):
                with self.assertRaisesRegex(CodegenError, "AES did not finish"):
                    wait_for_aes(root, timeout=1)
            self.assertIn("aes_hunt.request", out.getvalue())


if __name__ == "__main__":
    unittest.main()
