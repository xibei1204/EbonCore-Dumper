"""Bounded readers for standard Unreal Pak v8/v10-v12 and IoStore v3-v8.

The format layout was checked against Epic's public format conventions and the
CUE4Parse format readers (https://github.com/FabianFG/CUE4Parse).  These are
independent Python readers, not an Unreal asset deserializer. IoStore extraction
returns raw chunks; cooked package reconstruction requires a separate tool.
"""
from __future__ import annotations

import hashlib
import os
from pathlib import Path, PurePosixPath
import struct
from dataclasses import asdict, dataclass
import tempfile
from typing import Callable, Iterator
import zlib


MAX_INDEX = 256 * 1024 * 1024
MAX_ENTRIES = 5_000_000
PAK_MAGIC = 0x5A6F12E1
UTOC_MAGIC = b"-==--==--==--==-"


class ContainerError(ValueError):
    """Unsupported, encrypted, malformed, or inconsistent container data."""


class _Reader:
    def __init__(self, data: bytes, position: int = 0):
        self.data, self.pos = data, position

    def read(self, size: int) -> bytes:
        if size < 0 or self.pos < 0 or self.pos + size > len(self.data):
            raise ContainerError("truncated container/index")
        value = self.data[self.pos:self.pos + size]
        self.pos += size
        return value

    def unpack(self, fmt: str):
        return struct.unpack("<" + fmt, self.read(struct.calcsize("<" + fmt)))[0]

    def count(self, maximum: int = MAX_ENTRIES) -> int:
        count = self.unpack("i")
        if not 0 <= count <= maximum:
            raise ContainerError("invalid or excessive array count")
        return count

    def boolean(self) -> bool:
        value = self.unpack("i")
        if value not in (0, 1):
            raise ContainerError("invalid serialized boolean (wrong key or format)")
        return bool(value)

    def string(self) -> str:
        length = self.unpack("i")
        if length == 0:
            return ""
        if abs(length) > 1024 * 1024:
            raise ContainerError("invalid FString length (wrong key or format)")
        wide = length < 0
        raw = self.read(abs(length) * (2 if wide else 1))
        terminator = b"\0\0" if wide else b"\0"
        if not raw.endswith(terminator):
            raise ContainerError("FString lacks terminator (wrong key or format)")
        try:
            result = raw[:-len(terminator)].decode("utf-16-le" if wide else "utf-8")
        except UnicodeError as error:
            raise ContainerError("invalid FString encoding") from error
        if "\0" in result:
            raise ContainerError("embedded NUL in FString")
        return result


def _read_at(path: Path, offset: int, size: int, maximum: int = MAX_INDEX) -> bytes:
    if not 0 <= size <= maximum or offset < 0 or offset + size > path.stat().st_size:
        raise ContainerError("container range out of bounds or above size limit")
    with path.open("rb") as stream:
        stream.seek(offset)
        data = stream.read(size)
    if len(data) != size:
        raise ContainerError("container changed or read was truncated")
    return data


def _key_bytes(key: bytes | str | None) -> bytes | None:
    if key is None:
        return None
    if isinstance(key, str):
        try:
            key = bytes.fromhex(key.removeprefix("0x").strip())
        except ValueError as error:
            raise ContainerError("AES key must contain 64 hexadecimal digits") from error
    if len(key) != 32:
        raise ContainerError("AES-256 requires a 32-byte key")
    return key


def _decrypt(data: bytes, key: bytes | None) -> bytes:
    if key is None:
        raise ContainerError("encrypted container: supply its verified AES-256 key")
    if len(data) % 16:
        raise ContainerError("encrypted region is not aligned to 16 bytes")
    try:
        from Crypto.Cipher import AES
    except ImportError as error:
        raise ContainerError("AES requires optional pycryptodome; unencrypted inventory needs only Python") from error
    return AES.new(key, AES.MODE_ECB).decrypt(data)


def _hash_matches(data: bytes, digest: bytes) -> bool:
    # Some writers include AES zero padding in IndexSize but hash before padding.
    if hashlib.sha1(data).digest() == digest:
        return True
    for pad in range(1, min(15, len(data)) + 1):
        if data[-pad:] != b"\0" * pad:
            break
        if hashlib.sha1(data[:-pad]).digest() == digest:
            return True
    return False


def _mount_path(mount: str) -> str:
    mount = mount.replace("\\", "/")
    # Unreal's mount point is commonly ../../../; only this trusted mount prefix
    # is stripped. Entry-relative ../ is always rejected by _relative_path.
    while mount.startswith("../"):
        mount = mount[3:]
    if mount.strip("/") == "":
        return ""
    return _relative_path(mount, allow_empty=True).rstrip("/")


def _relative_path(value: str, allow_empty: bool = False) -> str:
    value = value.replace("\\", "/")
    if not value and allow_empty:
        return ""
    parts = value.rstrip("/").split("/")
    if not value or value.startswith("/") or any(p in ("", ".", "..") for p in parts):
        raise ContainerError("unsafe or empty resource path")
    reserved = {"CON", "PRN", "AUX", "NUL", *(f"COM{i}" for i in range(1, 10)), *(f"LPT{i}" for i in range(1, 10))}
    for part in parts:
        if any(ord(c) < 32 or c in ':<>"|?*' for c in part) or part.endswith((".", " ")) or part.split(".")[0].upper() in reserved:
            raise ContainerError("unsafe Windows resource path")
    return "/".join(parts)


def _destination(source: Path, output_root: Path | str, relative: str) -> Path:
    root = Path(output_root).resolve()
    source = source.resolve()
    protected = source.parent
    for ancestor in source.parents:
        if ancestor.name.casefold() == "neverness to everness":
            protected = ancestor
            break
        if ancestor.name.casefold() == "windowsnoeditor":
            protected = ancestor
    if root == protected or root.is_relative_to(protected) or source.is_relative_to(root):
        raise ContainerError("output must be separate from the input/client installation")
    dest = (root / _relative_path(relative)).resolve()
    if not dest.is_relative_to(root):
        raise ContainerError("resource path escapes output root")
    return dest


def _write_output(destination: Path, data: bytes, overwrite: bool) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists() and not overwrite:
        raise ContainerError("output already exists; enable overwrite explicitly")
    fd, name = tempfile.mkstemp(prefix=".ebon-", suffix=".part", dir=destination.parent)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(data)
        if overwrite:
            os.replace(name, destination)
        else:
            # hard-link publication is exclusive: no accidental overwrite race.
            os.link(name, destination)
            os.unlink(name)
    finally:
        if os.path.exists(name):
            os.unlink(name)


def _decompress(method: str, data: bytes, expected: int, callback=None) -> bytes:
    if method.casefold() in ("none", ""):
        output = data
    elif method.casefold() == "zlib":
        decoder = zlib.decompressobj()
        output = decoder.decompress(data, expected + 1)
        if not decoder.eof or decoder.unused_data or decoder.unconsumed_tail:
            raise ContainerError("invalid, trailing, or oversized zlib block")
    elif method.casefold() == "gzip":
        decoder = zlib.decompressobj(16 + zlib.MAX_WBITS)
        output = decoder.decompress(data, expected + 1)
        if not decoder.eof or decoder.unused_data or decoder.unconsumed_tail:
            raise ContainerError("invalid, trailing, or oversized gzip block")
    elif callback is not None:
        output = callback(method, data, expected)
    else:
        raise ContainerError(f"unsupported compression {method!r}; provide a compatible decompressor")
    if len(output) != expected:
        raise ContainerError("decompressed block size differs from index")
    return output


@dataclass(frozen=True)
class PakEntry:
    path: str
    offset: int
    size: int
    uncompressed_size: int
    compression: str
    encrypted: bool
    blocks: tuple[tuple[int, int], ...]
    block_size: int
    header_size: int
    deleted: bool = False
    sha1: str | None = None

    def to_dict(self) -> dict:
        return asdict(self)


class PakFile:
    def __init__(self, path: str | Path, key: bytes | str | None = None, decompressor: Callable | None = None):
        self.path = Path(path).resolve()
        self.key = _key_bytes(key)
        self.decompressor = decompressor
        self._entries: list[PakEntry] | None = None
        self.mount_point = ""
        self.info = self._footer()

    def _footer(self) -> dict:
        size = self.path.stat().st_size
        tail = _read_at(self.path, max(0, size - 1024), min(1024, size))
        magic = struct.pack("<I", PAK_MAGIC)
        pos = tail.rfind(magic)
        if pos < 0 or pos + 44 > len(tail):
            raise ContainerError("standard Pak footer not found")
        version, offset, index_size = struct.unpack_from("<Iqq", tail, pos + 4)
        if version not in (8, 10, 11, 12):
            raise ContainerError(f"unsupported Pak version {version}; supported: 8, 10, 11, 12")
        footer_size = 221
        if pos != len(tail) - 204 or pos < 17:
            raise ContainerError("unexpected Pak footer layout")
        encrypted = tail[pos - 1]
        if encrypted not in (0, 1):
            raise ContainerError("invalid Pak encrypted-index flag")
        if offset < 0 or index_size <= 0 or index_size > MAX_INDEX or offset + index_size > size - footer_size:
            raise ContainerError("Pak index range is invalid")
        self._index_hash = tail[pos + 24:pos + 44]
        methods = ["None"]
        for start in range(pos + 44, pos + 204, 32):
            try:
                methods.append(tail[start:start + 32].split(b"\0", 1)[0].decode("ascii"))
            except UnicodeError as error:
                raise ContainerError("invalid compression method table") from error
        self._methods = methods
        return {"kind": "pak", "path": str(self.path), "size": size, "version": version,
                "index_offset": offset, "index_size": index_size, "index_sha1": self._index_hash.hex(),
                "encrypted_index": bool(encrypted), "encryption_key_guid": tail[pos - 17:pos - 1].hex(),
                "compression_methods": [m for m in methods if m], "status": "encrypted_index" if encrypted and self.key is None else "inventory_only"}

    def _index_region(self, offset: int, size: int, digest: bytes) -> bytes:
        data = _read_at(self.path, offset, size)
        if self.info["encrypted_index"]:
            data = _decrypt(data, self.key)
        if not _hash_matches(data, digest):
            raise ContainerError("Pak index SHA-1 mismatch (wrong key, corruption, or custom encryption)")
        return data

    def _method(self, index: int) -> str:
        if index >= len(self._methods) or not self._methods[index]:
            raise ContainerError("invalid Pak compression method index")
        return self._methods[index]

    def _full_entry(self, reader: _Reader, path: str) -> PakEntry:
        start = reader.pos
        offset = reader.unpack("q")
        size = reader.unpack("q")
        raw_size = reader.unpack("q")
        method = self._method(reader.unpack("I"))
        digest = reader.read(20).hex()
        blocks = []
        if method != "None":
            for _ in range(reader.count(65535)):
                begin, end = reader.unpack("q"), reader.unpack("q")
                blocks.append((begin + offset, end + offset))
        flags, block_size = reader.unpack("B"), reader.unpack("I")
        if flags & ~3:
            raise ContainerError("unknown Pak entry flags")
        return PakEntry(path, offset, size, raw_size, method, bool(flags & 1), tuple(blocks), block_size, reader.pos - start, bool(flags & 2), digest)

    def _encoded_entry(self, data: bytes, location: int, path: str) -> PakEntry:
        reader = _Reader(data, location)
        flags = reader.unpack("I")
        block_size = reader.unpack("I") if flags & 63 == 63 else (flags & 63) << 11
        method = self._method((flags >> 23) & 63)
        offset = reader.unpack("I" if flags & (1 << 31) else "q")
        raw_size = reader.unpack("I" if flags & (1 << 30) else "q")
        size = raw_size if method == "None" else reader.unpack("I" if flags & (1 << 29) else "q")
        encrypted = bool(flags & (1 << 22))
        count = (flags >> 6) & 65535
        if method == "None" and count:
            raise ContainerError("uncompressed Pak entry has compression blocks")
        header_size = 53 + (4 + count * 16 if method != "None" else 0)
        begin = offset + header_size
        blocks = []
        for _ in range(count):
            length = size if count == 1 and not encrypted else reader.unpack("I")
            blocks.append((begin, begin + length))
            begin += (length + 15) & ~15 if encrypted else length
        if count == 1:
            block_size = raw_size
        return PakEntry(path, offset, size, raw_size, method, encrypted, tuple(blocks), block_size, header_size)

    def _validate_entry(self, entry: PakEntry) -> None:
        limit = self.info["index_offset"]
        if entry.deleted:
            return
        if entry.offset < 0 or entry.size < 0 or entry.uncompressed_size < 0 or entry.offset + entry.header_size > limit:
            raise ContainerError("invalid Pak entry extent")
        if entry.compression != "None":
            if entry.uncompressed_size and (not entry.blocks or entry.block_size <= 0):
                raise ContainerError("compressed entry lacks blocks")
            if any(a < entry.offset + entry.header_size or b <= a or b > limit for a, b in entry.blocks):
                raise ContainerError("compressed block outside Pak data area")
            if sum(b - a for a, b in entry.blocks) != entry.size:
                raise ContainerError("compressed block sizes differ from entry size")
        elif entry.offset + entry.header_size + ((entry.size + 15) & ~15 if entry.encrypted else entry.size) > limit:
            raise ContainerError("Pak data outside data area")

    def entries(self) -> list[PakEntry]:
        if self._entries is not None:
            return list(self._entries)
        data = self._index_region(self.info["index_offset"], self.info["index_size"], self._index_hash)
        reader = _Reader(data)
        self.mount_point = reader.string()
        mount = _mount_path(self.mount_point)
        count = reader.count()
        rows = []
        if self.info["version"] == 8:
            for _ in range(count):
                name = _relative_path(reader.string())
                path = f"{mount}/{name}" if mount else name
                rows.append(self._full_entry(reader, path))
        else:
            reader.read(8)  # path-hash seed
            if reader.boolean():
                reader.read(36)  # path-hash index; full directory provides names
            if not reader.boolean():
                raise ContainerError("Pak has no full directory index; name enumeration unavailable")
            directory_offset, directory_size = reader.unpack("q"), reader.unpack("q")
            directory_hash = reader.read(20)
            encoded = reader.read(reader.count(MAX_INDEX))
            non_encoded = [self._full_entry(reader, "") for _ in range(reader.count())]
            directory = _Reader(self._index_region(directory_offset, directory_size, directory_hash))
            for _ in range(directory.count()):
                prefix = directory.string().replace("\\", "/")
                if prefix.startswith("/"):
                    prefix = prefix[1:]
                if prefix:
                    _relative_path(prefix)
                for _ in range(directory.count()):
                    name = _relative_path(directory.string())
                    path = _relative_path("/".join(p for p in (mount, prefix.rstrip("/"), name) if p))
                    location = directory.unpack("i")
                    if location == -2147483648:  # deleted file marker
                        continue
                    if location >= 0:
                        row = self._encoded_entry(encoded, location, path)
                    else:
                        index = -location - 1
                        if index >= len(non_encoded):
                            raise ContainerError("invalid non-encoded Pak entry reference")
                        row = PakEntry(**{**non_encoded[index].to_dict(), "path": path})
                    rows.append(row)
                    if len(rows) > count:
                        raise ContainerError("directory contains more files than declared")
        seen = set()
        for row in rows:
            self._validate_entry(row)
            identity = row.path.casefold()
            if identity in seen:
                raise ContainerError("duplicate/case-colliding Pak paths")
            seen.add(identity)
        self._entries = rows
        self.info.update(status="index_read", mount_point=self.mount_point, declared_entries=count, listed_entries=len(rows))
        return list(rows)

    def inventory(self) -> list[dict]:
        return [entry.to_dict() for entry in self.entries()]

    def read_entry(self, entry: PakEntry, max_bytes: int = 512 * 1024 * 1024) -> bytes:
        if self._entries is None:
            self.entries()
        if entry not in self._entries:
            raise ContainerError("entry does not belong to this Pak")
        if entry.deleted:
            raise ContainerError("cannot extract deleted Pak entry")
        if entry.uncompressed_size > max_bytes:
            raise ContainerError("resource exceeds maximum extraction size")
        self._validate_entry(entry)
        if entry.compression == "None":
            read_size = (entry.size + 15) & ~15 if entry.encrypted else entry.size
            data = _read_at(self.path, entry.offset + entry.header_size, read_size, max(max_bytes + 16, MAX_INDEX))
            if entry.encrypted:
                data = _decrypt(data, self.key)
            data = data[:entry.uncompressed_size]
        else:
            output = bytearray()
            for begin, end in entry.blocks:
                length = end - begin
                data = _read_at(self.path, begin, (length + 15) & ~15 if entry.encrypted else length, max(max_bytes + 16, MAX_INDEX))
                if entry.encrypted:
                    data = _decrypt(data, self.key)
                expected = min(entry.block_size, entry.uncompressed_size - len(output))
                output.extend(_decompress(entry.compression, data[:length], expected, self.decompressor))
            data = bytes(output)
        if len(data) != entry.uncompressed_size:
            raise ContainerError("extracted resource length differs from index")
        if entry.sha1 and entry.sha1 != "0" * 40 and hashlib.sha1(data).hexdigest() != entry.sha1:
            # Unreal stores a hash of compressed data for compressed entries.
            if entry.compression == "None":
                raise ContainerError("Pak resource SHA-1 mismatch")
        return data

    def extract(self, entry: PakEntry, output_root: str | Path, overwrite: bool = False, max_bytes: int = 512 * 1024 * 1024) -> Path:
        destination = _destination(self.path, output_root, entry.path)
        _write_output(destination, self.read_entry(entry, max_bytes), overwrite)
        return destination


class UtocFile:
    """Inventory and raw chunk access; .ucas bytes are never called .uasset."""
    def __init__(self, path: str | Path, key: bytes | str | None = None, decompressor: Callable | None = None):
        self.path = Path(path).resolve()
        self.key, self.decompressor = _key_bytes(key), decompressor
        self._data = _read_at(self.path, 0, self.path.stat().st_size)
        self.info = self._header()

    def _header(self) -> dict:
        reader = _Reader(self._data)
        if reader.read(16) != UTOC_MAGIC:
            raise ContainerError("invalid IoStore TOC magic")
        version = reader.unpack("B")
        reader.read(3)
        if not 3 <= version <= 8:
            raise ContainerError(f"unsupported IoStore version {version}; supported 3-8")
        fields = struct.unpack("<9I", reader.read(36))
        header_size, count, block_count, block_entry_size, method_count, method_length, compression_block_size, directory_size, partition_count = fields
        container_id, guid = reader.read(8).hex(), reader.read(16).hex()
        flags = reader.unpack("B")
        reader.read(3)
        seeds, partition_size, overflow = reader.unpack("I"), reader.unpack("Q"), reader.unpack("I")
        if header_size != 144 or block_entry_size != 12 or flags & ~31:
            raise ContainerError("unsupported IoStore header/block layout")
        if count > MAX_ENTRIES or block_count > MAX_ENTRIES or method_count > 64 or not 1 <= method_length <= 256 or partition_count > 10000:
            raise ContainerError("excessive IoStore counts")
        if (count and compression_block_size == 0) or partition_size == 0:
            raise ContainerError("zero IoStore block or partition size")
        self._count, self._block_count = count, block_count
        self._ids_offset = header_size
        self._offsets_offset = header_size + count * 12
        self._blocks_offset = header_size + count * 22 + (seeds if version >= 4 else 0) * 4 + (overflow if version >= 5 else 0) * 4
        methods_offset = self._blocks_offset + block_count * block_entry_size
        r = _Reader(self._data, methods_offset)
        methods = ["None"]
        for _ in range(method_count):
            try:
                methods.append(r.read(method_length).split(b"\0", 1)[0].decode("ascii"))
            except UnicodeError as error:
                raise ContainerError("invalid IoStore compression method") from error
        if flags & 4:
            signature_size = r.unpack("i")
            if not 1 <= signature_size <= 4096:
                raise ContainerError("invalid IoStore signature size")
            r.read(signature_size * 2 + block_count * 20)
        self._directory_offset = r.pos
        r.read(directory_size)
        self._meta_offset = r.pos
        meta_size = 24 if version >= 8 else 21
        r.read(count * meta_size)
        self._methods = methods
        return {"kind": "utoc", "path": str(self.path), "size": len(self._data), "version": version,
                "header_size": header_size, "chunk_count": count, "compression_block_count": block_count,
                "compression_block_size": compression_block_size, "compression_methods": methods,
                "directory_index_size": directory_size, "partition_count": partition_count, "partition_size": partition_size,
                "container_id": container_id, "encryption_key_guid": guid, "flags": flags,
                "encrypted": bool(flags & 2), "signed": bool(flags & 4), "indexed": bool(flags & 8),
                "status": "chunk_inventory", "signature_verified": False,
                "package_reconstruction": "not_implemented"}

    def chunks(self) -> Iterator[dict]:
        for index in range(self._count):
            start = self._ids_offset + index * 12
            chunk_id = self._data[start:start + 12]
            start = self._offsets_offset + index * 10
            offset = int.from_bytes(self._data[start:start + 5], "big")
            length = int.from_bytes(self._data[start + 5:start + 10], "big")
            if offset + length > self._block_count * self.info["compression_block_size"]:
                raise ContainerError("IoStore chunk exceeds logical compression stream")
            yield {"index": index, "chunk_id": chunk_id.hex(), "chunk_type": chunk_id[11], "offset": offset, "length": length}

    def inventory(self) -> list[dict]:
        return list(self.chunks())

    def directories(self) -> list[dict]:
        if not self.info["indexed"] or not self.info["directory_index_size"]:
            return []
        size = self.info["directory_index_size"]
        data = self._data[self._directory_offset:self._directory_offset + size]
        if self.info["encrypted"]:
            data = _decrypt(data, self.key)
        r = _Reader(data)
        mount = _mount_path(r.string())
        dirs = [struct.unpack("<4I", r.read(16)) for _ in range(r.count())]
        files = [struct.unpack("<3I", r.read(12)) for _ in range(r.count())]
        names = [r.string() for _ in range(r.count())]
        invalid = 0xFFFFFFFF
        result, visited_dirs, visited_files = [], set(), set()
        pending = [(0, mount)] if dirs else []
        while pending:
            index, parent = pending.pop()
            if index == invalid:
                continue
            if index >= len(dirs) or index in visited_dirs:
                raise ContainerError("IoStore directory cycle or invalid index")
            visited_dirs.add(index)
            name, first_child, sibling, first_file = dirs[index]
            if index == 0:
                prefix = parent
            else:
                if name >= len(names):
                    raise ContainerError("invalid IoStore directory name index")
                prefix = "/".join(p for p in (parent, _relative_path(names[name])) if p)
            pending.extend([(sibling, parent), (first_child, prefix)])
            current = first_file
            while current != invalid:
                if current >= len(files) or current in visited_files:
                    raise ContainerError("IoStore file cycle or invalid index")
                visited_files.add(current)
                name, next_file, user_data = files[current]
                if name >= len(names) or user_data >= self._count:
                    raise ContainerError("invalid IoStore file/chunk reference")
                path = _relative_path("/".join(p for p in (prefix, names[name]) if p))
                result.append({"path": path, "chunk_index": user_data})
                current = next_file
        return result

    def read_chunk(self, index: int, max_bytes: int = 512 * 1024 * 1024) -> bytes:
        if not 0 <= index < self._count:
            raise ContainerError("invalid chunk index")
        start = self._offsets_offset + index * 10
        offset = int.from_bytes(self._data[start:start + 5], "big")
        length = int.from_bytes(self._data[start + 5:start + 10], "big")
        if length > max_bytes:
            raise ContainerError("chunk exceeds maximum extraction size")
        block_size = self.info["compression_block_size"]
        output = bytearray()
        first, last = offset // block_size, (offset + length + block_size - 1) // block_size
        if last > self._block_count:
            raise ContainerError("chunk exceeds compression stream")
        for block_index in range(first, last):
            r = _Reader(self._data, self._blocks_offset + block_index * 12)
            raw = r.read(12)
            packed_offset_size = int.from_bytes(raw[:8], "little")
            packed_raw_method = int.from_bytes(raw[8:12], "little")
            physical = packed_offset_size & ((1 << 40) - 1)
            compressed_size = (packed_offset_size >> 40) & 0xFFFFFF
            raw_size = packed_raw_method & 0xFFFFFF
            method_index = packed_raw_method >> 24
            if method_index >= len(self._methods) or raw_size > block_size or compressed_size > max_bytes:
                raise ContainerError("invalid IoStore compression block")
            partition, physical = divmod(physical, self.info["partition_size"])
            if partition >= self.info["partition_count"]:
                raise ContainerError("invalid IoStore partition")
            suffix = "" if partition == 0 else f"_s{partition}"
            ucas = self.path.with_name(self.path.stem + suffix + ".ucas")
            read_size = (compressed_size + 15) & ~15 if self.info["encrypted"] else compressed_size
            try:
                compressed = _read_at(ucas, physical, read_size, max(max_bytes + 16, MAX_INDEX))
            except FileNotFoundError as error:
                raise ContainerError(f"missing IoStore partition {ucas.name}") from error
            if self.info["encrypted"]:
                compressed = _decrypt(compressed, self.key)
            output.extend(_decompress(self._methods[method_index], compressed[:compressed_size], raw_size, self.decompressor))
        relative = offset - first * block_size
        data = bytes(output[relative:relative + length])
        if len(data) != length:
            raise ContainerError("raw chunk size differs from TOC")
        return data

    def extract_chunk(self, index: int, output_root: str | Path, overwrite: bool = False, max_bytes: int = 512 * 1024 * 1024) -> Path:
        chunk_id = self._data[self._ids_offset + index * 12:self._ids_offset + (index + 1) * 12].hex()
        destination = _destination(self.path, output_root, f"chunks/{chunk_id}.bin")
        _write_output(destination, self.read_chunk(index, max_bytes), overwrite)
        return destination


def inspect_container(path: str | Path) -> dict:
    path = Path(path)
    if path.suffix.casefold() == ".pak":
        reader = PakFile(path)
        if reader.info["encrypted_index"]:
            return dict(reader.info)
        return {**reader.info, "entries": reader.inventory()}
    if path.suffix.casefold() == ".utoc":
        reader = UtocFile(path)
        return {**reader.info, "chunks": reader.inventory()}
    raise ContainerError("expected .pak or .utoc")
