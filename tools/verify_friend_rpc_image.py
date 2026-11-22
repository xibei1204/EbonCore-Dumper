"""Export version-bound Friend RPC and parameter serializer code evidence.

Static inspection only: this does not execute the image or verify packet bytes.
Requires the optional pefile and Capstone dependencies in requirements.txt.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct

import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64


MODULE_SHA256 = "64ed3cae0b5fc61f3b56cdd933b4807cb23f0ec7913590db60d1a8642de2dfec"
DECODED_SHA256 = "94e38635dcc1168ea3d01ba4c85fdbfc99db3cb4abadc332f8d18b7f597aa3ef"


def digest(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def verify(image: Path, original: Path) -> dict:
    original_hash, decoded_hash = digest(original), digest(image)
    if original_hash != MODULE_SHA256 or decoded_hash != DECODED_SHA256:
        raise ValueError("unsupported executable identity; re-analyze a changed build")
    pe = pefile.PE(str(image), fast_load=True)
    source = pefile.PE(str(original), fast_load=True)
    shape = lambda p: [(s.VirtualAddress, s.Misc_VirtualSize, s.PointerToRawData, s.SizeOfRawData)
                       for s in p.sections]
    if (pe.FILE_HEADER.Machine != 0x8664 or shape(pe) != shape(source) or
            pe.OPTIONAL_HEADER.ImageBase != source.OPTIONAL_HEADER.ImageBase or
            pe.OPTIONAL_HEADER.SizeOfImage != source.OPTIONAL_HEADER.SizeOfImage):
        raise ValueError("decoded/original PE layouts differ")
    base = pe.OPTIONAL_HEADER.ImageBase

    def read(rva, size):
        for section in pe.sections:
            offset = rva - section.VirtualAddress
            if 0 <= offset and offset + size <= section.SizeOfRawData:
                with image.open("rb") as stream:
                    stream.seek(section.PointerToRawData + offset)
                    value = stream.read(size)
                if len(value) == size:
                    return value
                break
        raise ValueError("requested evidence is not backed by complete file bytes")

    def rip(lea):
        value = read(lea, 7)
        if value[0] not in (0x48, 0x4c) or value[1] != 0x8d or value[2] & 0xc7 != 5:
            raise ValueError("registration reference is not a RIP-relative LEA")
        return lea + 7 + struct.unpack_from("<i", value, 3)[0]

    def pointer(rva):
        return struct.unpack("<Q", read(rva, 8))[0] - base

    # Follow actual references instead of treating a nearby name as ownership.
    if rip(0x6bc9798) != 0xd102198 or rip(0x6bc979f) != 0x6be9140:
        raise ValueError("HottaRpcMessage StaticStruct references changed")
    if read(0xd102198, 32) != "HottaRpcMessage\0".encode("utf-16le"):
        raise ValueError("HottaRpcMessage reflected name differs")
    registration = rip(0x6be9150)
    if registration != 0x10014be8 or pointer(registration + 16) != 0x6bc97c0:
        raise ValueError("HottaRpcMessage ops constructor differs")
    hotta_ops = rip(0x6bc97d3)
    array_ops, wrapper_ops = rip(0x117d03e), rip(0x117d0ee)
    expected = ((hotta_ops, 0x14ac640, 0x14ac640),
                (array_ops, 0x6bca550, 0x6bca570),
                (wrapper_ops, 0x6bca780, 0x6bca800))
    for ops, serialize, netserialize in expected:
        if pointer(ops + 0x30) != serialize or pointer(ops + 0x50) != netserialize:
            raise ValueError("struct ops serializer binding changed")
    if read(0x14ac640, 3) != b"\x32\xc0\xc3":
        raise ValueError("unsupported serializer stub changed")

    md = Cs(CS_ARCH_X86, CS_MODE_64)
    regions = []
    # Explicit ranges include chained exception regions, not just the first
    # RUNTIME_FUNCTION entry, which can end before a serializer's real return.
    spans = (
        ("Client_Friend_RPC_exec", 0x99f1020, 0x99f10d2),
        ("Server_Friend_RPC_exec", 0x9a27080, 0x9a27132),
        ("HottaRpcMessage_StaticStruct", 0x6bc9780, 0x6bc97f5),
        ("HottaRpcMessage_registration", 0x6be9140, 0x6be916f),
        ("ParameterWrapperArray_ops_registration", 0x117cfd0, 0x117d07e),
        ("ParameterWrapper_ops_registration", 0x117d080, 0x117d12e),
        ("ParameterWrapperArray_ops_serializers", 0x6bca550, 0x6bca585),
        ("ParameterWrapper_ops_serializers", 0x6bca780, 0x6bca815),
        ("ParameterWrapperArray_archive_helper", 0x6bdc7c0, 0x6bdc8f7),
        ("ParameterWrapper_archive_helper", 0x6bdc130, 0x6bdc7b2),
        ("ByteArray_archive_helper", 0x155c340, 0x155c488),
        ("archive_scalar_byte_swap", 0x1674940, 0x167499d),
        ("HottaRpcMessage_ops_capabilities", 0x170de50, 0x170de69),
        ("Wrapper_ops_capabilities", 0x57fb330, 0x57fb349),
        ("PrepareCppStructOps", 0x179b2e0, 0x179bc90),
        ("StructProperty_NetSerialize_consumer", 0x18c7000, 0x18c710d),
        ("NetSerializeStruct_dispatch", 0x4223610, 0x4223908),
        ("RepLayout_InitFromProperty", 0x47befb0, 0x47bf5a9),
        ("RepLayout_InitFromStruct", 0x47bf9d0, 0x47bfbd2),
        ("FieldIterator_including_super", 0x1735030, 0x173510b),
        ("SuperStruct_accessor", 0x17156f0, 0x17156f5),
        ("Property_offset_sort_excerpt", 0x177ba9d, 0x177bab2),
        ("RPC_ReceiveProperties", 0x47eb330, 0x47eba2b),
        ("RPC_array_count_and_elements", 0x47f7f80, 0x47f8195),
        ("RPC_recursive_element", 0x47f81a0, 0x47f83d9),
        ("ReceivedRPC_callsite_excerpt", 0x422e5ac, 0x422e5e5),
        ("IntProperty_registration", 0x18a5dd0, 0x18a5e74),
        ("IntProperty_constructor", 0x1886cd0, 0x1886e33),
        ("IntProperty_NetSerialize_bridge", 0x186d770, 0x186d7e9),
        ("IntProperty_SerializeItem", 0x17a2e40, 0x17a2e8b),
    )
    for label, begin, end in spans:
        data = read(begin, end - begin)
        regions.append(dict(label=label, rva=hex(begin), size=len(data), bytes=data.hex(),
                            sha256=hashlib.sha256(data).hexdigest(),
                            instructions=[dict(rva=hex(i.address-base), bytes=i.bytes.hex(),
                                               instruction=i.mnemonic + " " + i.op_str)
                                          for i in md.disasm(data, base+begin)]))
    ops_records = []
    for label, ops in (("HottaRpcMessage", hotta_ops), ("ParameterWrapperArray", array_ops),
                       ("ParameterWrapper", wrapper_ops)):
        ops_records.append(dict(type=label, vtable_rva=hex(ops), bytes=read(ops, 0xe8).hex(),
                                serialize_rva=hex(pointer(ops+0x30)),
                                netserialize_rva=hex(pointer(ops+0x50)),
                                capabilities_rva=hex(pointer(ops+8))))
    return dict(format="EbonFriendRpcImage-v1", original_image_sha256=original_hash,
                decoded_image_sha256=decoded_hash, image_base=hex(base),
                evidence_kind="static_code", struct_ops_binding_verified=True,
                normal_rpc_parameter_order=["InMessage_presence_bit", "ParameterWrappers:uint16_count_and_elements",
                                            "ReadParameterIndex:int32", "Type:int32"],
                wire_packet_bytes_captured=False, packet_mapping_verified=False,
                friend_submessage_encoding_verified=False, regions=regions, struct_ops=ops_records,
                limitations=[
                    "Archive helpers are code evidence, not captured RPC packets.",
                    "HottaRpcMessage has its own ops with false serializer stubs; do not apply the base helper as the complete RPC format.",
                    "FName uses the archive's virtual serializer; its runtime network representation remains unverified.",
                    "Object/ObjectArray can use PackageMap instead of emitting BinaryData verbatim.",
                    "Friend message Type values, RPC bit framing and payload semantics need runtime correlation."])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--module-image", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    if args.out.resolve() in (args.image.resolve(), args.module_image.resolve()) or args.out.exists():
        parser.error("output must be a new file distinct from the input images")
    try:
        result = verify(args.image, args.module_image)
        args.out.parent.mkdir(parents=True, exist_ok=True)
        with args.out.open("x", encoding="utf-8") as stream:
            json.dump(result, stream, ensure_ascii=False, indent=2)
            stream.write("\n")
    except (OSError, ValueError, pefile.PEFormatError) as error:
        parser.exit(1, str(error) + "\n")
    print(json.dumps({key: value for key, value in result.items()
                      if key not in ("regions", "struct_ops")}, indent=2))


if __name__ == "__main__":
    main()
