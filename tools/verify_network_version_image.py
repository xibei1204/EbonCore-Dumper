"""Correlate a decoded PE's UE checksum branch with a version-bound original PE.

No code execution and no claim about the value of unsaved runtime globals.
Requires optional pefile/Capstone; the runtime dumper itself does not.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct

import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream,"sha256").hexdigest()


def verify(image: Path, original: Path, profile: Path) -> dict:
    config={}
    for line in profile.read_text(encoding="utf-8-sig").splitlines():
        line=line.strip()
        if not line or line.startswith(("#",";","[")):continue
        key,value=line.split("=",1)
        if key.strip() in config:raise ValueError("duplicate profile key")
        config[key.strip()]=value.strip()
    original_hash=digest(original)
    if config.get("exe_sha256")!=original_hash:raise ValueError("original executable SHA-256 differs from profile")
    pe=pefile.PE(str(image),fast_load=True);source=pefile.PE(str(original),fast_load=True)
    shape=lambda p:[(s.VirtualAddress,s.Misc_VirtualSize,s.PointerToRawData,s.SizeOfRawData) for s in p.sections]
    if shape(pe)!=shape(source) or pe.OPTIONAL_HEADER.SizeOfImage!=source.OPTIONAL_HEADER.SizeOfImage:
        raise ValueError("decoded and original executable layouts differ")
    getter=int(config["network_version_getter_rva"],0)
    hello=int(config["network_version_hello_call_rva"],0)
    def read(rva,size):
        for section in pe.sections:
            delta=rva-section.VirtualAddress
            if 0<=delta and delta+size<=section.SizeOfRawData:
                with image.open("rb") as stream:
                    stream.seek(section.PointerToRawData+delta);data=stream.read(size)
                if len(data)!=size:raise ValueError("truncated code")
                return data
        raise ValueError("requested range is not backed by PE file bytes")
    g=read(getter,24);h=read(hello,9)
    if (g[:3]!=bytes.fromhex("4883ec") or g[4:6]!=bytes.fromhex("803d") or
            g[10:15]!=bytes.fromhex("00740b8b05") or g[19:22]!=bytes.fromhex("4883c4") or
            g[22]!=g[3] or g[23]!=0xc3 or h[0]!=0xe8 or h[5:]!=bytes.fromhex("89442428")):
        raise ValueError("getter/Hello code does not have the validated shape")
    flag=getter+11+struct.unpack_from("<i",g,6)[0]
    cache=getter+19+struct.unpack_from("<i",g,15)[0]
    if cache!=flag+4 or hello+5+struct.unpack_from("<i",h,1)[0]!=getter:
        raise ValueError("getter cache or Hello call target mismatch")
    md=Cs(CS_ARCH_X86,CS_MODE_64);md.detail=True
    base=pe.OPTIONAL_HEADER.ImageBase
    regions=[]
    # Detailed body/CRC/serialization spans were located by UTF-16 RIP xrefs.
    for label,begin,end in (("getter",getter,0x16416ed),("crc",0x1529e90,0x1529efe),
                            ("hello_version_source",0x60a7598,0x60a75b9),
                            ("hello_serialization",0x60a76b6,0x60a77f8)):
        if not 0<end-begin<=4096:raise ValueError("unsupported function body binding")
        data=read(begin,end-begin)
        rows=[dict(rva=hex(i.address-base),bytes=i.bytes.hex(),instruction=i.mnemonic+" "+i.op_str)
              for i in md.disasm(data,base+begin)]
        regions.append(dict(label=label,rva=hex(begin),bytes=data.hex(),instructions=rows))
    return dict(format="EbonNetworkVersionImage-v1",original_image_sha256=original_hash,
                decoded_image_sha256=digest(image),image_base=hex(base),
                getter_rva=hex(getter),hello_call_rva=hex(hello),cache_flag_rva=hex(flag),cache_value_rva=hex(cache),
                function_binding_verified=True,runtime_value=None,runtime_value_verified=False,
                note="cached globals are virtual/runtime data; disk code does not prove their current value",regions=regions)


if __name__=="__main__":
    parser=argparse.ArgumentParser()
    parser.add_argument("--image",type=Path,required=True)
    parser.add_argument("--module-image",type=Path,required=True)
    parser.add_argument("--profile",type=Path,required=True)
    parser.add_argument("--out",type=Path,required=True)
    args=parser.parse_args()
    result=verify(args.image,args.module_image,args.profile)
    args.out.parent.mkdir(parents=True,exist_ok=True)
    with args.out.open("x",encoding="utf-8") as stream:json.dump(result,stream,ensure_ascii=False,indent=2)
    print(json.dumps({key:value for key,value in result.items() if key!="regions"},indent=2))
