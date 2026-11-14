"""Generate sized Unreal declarations and ProcessEvent wrappers from reflection.

Unknown semantic types remain explicit unresolved storage, never guessed names.
Every emitted nonempty layout has sizeof/alignof/member-offset assertions.
"""
from __future__ import annotations

from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import re

from .sdk_codegen import CodegenError, _cpp_string, _validate_dump, _jsonl
from .network_codegen import generate_network_files

KEYWORDS = set("alignas alignof and asm auto bitand bitor bool break case catch char char16_t char32_t class compl concept const consteval constexpr constinit const_cast continue co_await co_return co_yield decltype default delete do double dynamic_cast else enum explicit export extern false float for friend goto if inline int long mutable namespace new noexcept not nullptr operator or private protected public register reinterpret_cast requires return short signed sizeof static static_assert static_cast struct switch template this thread_local throw true try typedef typeid typename union unsigned using virtual void volatile wchar_t while xor final override".split())
KEYWORDS.update('NULL TRUE FALSE ERROR IN OUT OPTIONAL min max GetObject GetClassName SendMessage CreateEvent DeleteFile DrawText GetCurrentTime GetMessage MemoryBarrier'.split())
PRIMITIVES = {"Int8Property": ("std::int8_t", 1), "ByteProperty": ("std::uint8_t", 1),
    "Int16Property": ("std::int16_t", 2), "UInt16Property": ("std::uint16_t", 2),
    "IntProperty": ("std::int32_t", 4), "UInt32Property": ("std::uint32_t", 4),
    "Int64Property": ("std::int64_t", 8), "UInt64Property": ("std::uint64_t", 8),
    "FloatProperty": ("float", 4), "DoubleProperty": ("double", 8)}


def identifier(text: str) -> str:
    result = re.sub(r"[^A-Za-z0-9_]", lambda m: f"_u{ord(m[0]):04X}", text)
    if not result or result[0].isdigit(): result = "N_" + result
    if result in KEYWORDS or result.startswith("__"): result = "SDK_" + result
    return result


def _unique(name: str, used: set[str]) -> str:
    candidate = name
    n = 1
    while candidate in used:
        candidate = f"{name}_{n}"
        n += 1
    used.add(candidate)
    return candidate


CORE = r'''#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace SDK {
struct UClass;
struct UFunction;
template<std::size_t N> struct TUnresolved { std::byte Data[N]; };
template<typename T, std::size_t N> using TStaticArray = std::array<T, N>;
struct FName { std::uint32_t ComparisonIndex; std::uint32_t Number; };
template<typename T> struct TArray {
    T* Data; std::int32_t Num; std::int32_t Max;
    bool IsValidIndex(std::int32_t i) const { return Data && i >= 0 && i < Num && Num <= Max; }
    T& operator[](std::int32_t i) { if(!IsValidIndex(i)) throw std::out_of_range("TArray"); return Data[i]; }
    const T& operator[](std::int32_t i) const { if(!IsValidIndex(i)) throw std::out_of_range("TArray"); return Data[i]; }
    T* begin() { return Data; } T* end() { return Num > 0 ? Data + Num : Data; }
};
using FString = TArray<char16_t>;
using FUtf8String = TArray<char>;
// Engine-owned opaque containers preserve observed storage; no allocator ABI is invented.
template<typename K, typename V, std::size_t N=80> struct TMap { std::byte Storage[N]; using KeyType=K; using ValueType=V; };
template<typename T, std::size_t N=80> struct TSet { std::byte Storage[N]; using ElementType=T; };
template<typename T, std::size_t N> struct TOptional { std::byte Storage[N]; using ValueType=T; };
template<typename T> struct TWeakObjectPtr { std::int32_t ObjectIndex; std::int32_t ObjectSerialNumber; };
template<typename T> struct TSubclassOf { UClass* Class; using ObjectType=T; operator UClass*() const { return Class; } };
template<typename T, std::size_t N=40> struct TSoftObjectPtr { std::byte Storage[N]; using ObjectType=T; };
template<typename T, std::size_t N=40> struct TSoftClassPtr { std::byte Storage[N]; using ObjectType=T; };
template<typename T, std::size_t N=24> struct TLazyObjectPtr { std::byte Storage[N]; using ObjectType=T; };
template<typename T> struct TScriptInterface { void* ObjectPointer; void* InterfacePointer; };
template<typename E, typename I> struct TEnumStorage {
    I Value;
    operator E() const { return static_cast<E>(Value); }
    TEnumStorage& operator=(E value) { Value=static_cast<I>(value); return *this; }
};
template<std::size_t N> struct TText { std::byte Storage[N]; };
struct FScriptDelegate { TWeakObjectPtr<void> Object; FName FunctionName; };
template<std::size_t N> struct TMulticastDelegate { std::byte Storage[N]; };
static_assert(sizeof(void*)==8, "This SDK requires a 64-bit process");
static_assert(sizeof(FName)==8 && sizeof(TArray<int>)==16);

namespace Runtime {
using FindObjectCallback = void*(*)(std::string_view);
using DispatchCallback = void(*)(void*, void*, void*);
void Initialize(std::uintptr_t moduleBase=0, std::int32_t processEventIndex=-1);
void SetCallbacks(FindObjectCallback findObject, DispatchCallback dispatch);
void RefreshObjectCache();
void* FindObject(std::string_view path);
void* GetDefaultObject(std::string_view classPath);
std::string ObjectName(const void* object);
std::string ObjectPath(const void* object);
void Invoke(void* object, std::string_view functionPath, void* parameters);
void DispatchEvent(void* object, void* function, void* parameters);
}
}
'''


class Generator:
    def __init__(self, state: dict, owners: list[dict]):
        self.state, self.owners = state, owners
        self.by_path = {o["path"]: o for o in owners}
        self.names: dict[str, str] = {}
        self.issues: list[dict] = []
        self.fields: dict[str, list[dict]] = {}
        self.dependencies: dict[str, set[str]] = defaultdict(set)
        self.methods: dict[str, list[dict]] = defaultdict(list)
        self.wrapper_count = 0
        self.enum_sizes: dict[str, int] = {}
        usage = defaultdict(Counter)
        for o in owners:
            for f in o["fields"]:
                d = f.get("detail") or {}
                target = d.get("target") or f.get("target")
                if target and f["type"] in {"EnumProperty", "ByteProperty"}: usage[target][f["element_size"]] += 1
        used: set[str] = {'FName','FString','FUtf8String','FScriptDelegate','TArray','TMap','TSet','TOptional','TUnresolved','Runtime'}
        def actor_type(o):
            seen=set()
            while o and o["path"] not in seen:
                if o["path"]=="/Script/Engine.Actor": return True
                seen.add(o["path"]); o=self.by_path.get(o.get("super"))
            return False
        for o in owners:
            prefix = {"class": "A" if actor_type(o) else "U", "struct": "F", "enum": "E", "function": "Params_"}[o["kind"]]
            raw = identifier(o["name"])
            name = raw if o["kind"]=="enum" and raw.startswith("E") else prefix+raw
            if name in used: name += "_"+hashlib.sha1(o["path"].encode()).hexdigest()[:8]
            self.names[o["path"]] = _unique(name, used)
            if o["kind"]=="enum":
                values=[v["value"] for v in o["enum_values"]] or [0]
                observed = usage[o["path"]]
                signed=min(values)<0
                minimum=next((n for n in (1,2,4,8) if -(1<<(n*8-1))<=min(values) and max(values)<1<<(n*8-1)),8) if signed else next((n for n in (1,2,4,8) if max(values)<1<<(n*8)),8)
                self.enum_sizes[o["path"]]=max(minimum,next(iter(observed))) if len(observed)==1 else minimum

    def issue(self, owner, field, reason):
        self.issues.append({"owner": owner, "field": field, "reason": reason})

    def type_name(self, detail: dict, size: int, owner: str, field: str) -> tuple[str,set[str]]:
        kind = detail.get("type", "Unknown")
        target = detail.get("target")
        target_owner = self.by_path.get(target)
        symbol = self.names.get(target)
        if kind in {"EnumProperty","ByteProperty"} and symbol and target_owner["kind"]=="enum" and size in (1,2,4,8):
            if self.enum_sizes[target]==size: return symbol,set()
            return f"TEnumStorage<{symbol}, std::uint{size*8}_t>",set()
        if kind in PRIMITIVES and PRIMITIVES[kind][1]==size: return PRIMITIVES[kind][0],set()
        if kind=="BoolProperty" and size==1 and detail.get("bool_mask") in (1,2,4,8,16,32,64,128,255): return "bool",set()
        if kind=="NameProperty" and size==8: return "FName",set()
        if kind=="StrProperty" and size==16: return "FString",set()
        if kind=="Utf8StrProperty" and size==16: return "FUtf8String",set()
        if kind=="TextProperty": return f"TText<{size}>",set()
        if kind=="StructProperty" and symbol and target_owner["kind"]=="struct" and target_owner["size"]==size:
            return symbol,{target}
        if kind in {"ObjectProperty","ClassProperty"} and size==8:
            if not symbol: self.issue(owner,field,"object target type was not recovered")
            if kind=='ClassProperty':return f"TSubclassOf<{symbol or 'UObject'}>",set()
            return f"{symbol or 'UObject'}*",set()
        if kind in {"SoftObjectProperty","SoftClassProperty","WeakObjectProperty","LazyObjectProperty","InterfaceProperty"}:
            if not symbol: self.issue(owner,field,"object target type was not recovered")
            symbol = symbol or "UObject"
            if kind=="WeakObjectProperty" and size==8: return f"TWeakObjectPtr<{symbol}>",set()
            if kind=="InterfaceProperty" and size==16: return f"TScriptInterface<{symbol}>",set()
            if kind=='SoftObjectProperty': return f"TSoftObjectPtr<{symbol}, {size}>",set()
            if kind=='SoftClassProperty': return f"TSoftClassPtr<{symbol}, {size}>",set()
            if kind=="LazyObjectProperty": return f"TLazyObjectPtr<{symbol}, {size}>",set()
        if kind=="DelegateProperty" and size==16: return "FScriptDelegate",set()
        if "Multicast" in kind and "Delegate" in kind: return f"TMulticastDelegate<{size}>",set()
        if kind in {"ArrayProperty","SetProperty","OptionalProperty"}:
            inner=detail.get("inner")
            if isinstance(inner,dict):
                inner_type,_=self.type_name(inner,inner.get("size",1),owner,field+"[]")
                if kind=="ArrayProperty" and size==16: return f"TArray<{inner_type}>",set()
                if kind=="SetProperty": return f"TSet<{inner_type}, {size}>",set()
                return f"TOptional<{inner_type}, {size}>",set()
        if kind=="MapProperty" and isinstance(detail.get("key"),dict) and isinstance(detail.get("value"),dict):
            key,_=self.type_name(detail['key'],detail['key'].get('size',1),owner,field+".key")
            value,_=self.type_name(detail['value'],detail['value'].get('size',1),owner,field+".value")
            return f"TMap<{key}, {value}, {size}>",set()
        self.issue(owner,field,f"unresolved {kind} ({size} bytes)")
        return f"TUnresolved<{size}>",set()

    def prepare_fields(self, o: dict, parameters=False) -> list[dict]:
        result=[];used={self.names[o['path']],"StaticClass","GetDefaultObject","SDKPath","GetName","GetPathName"}
        if o['path']=='/Script/CoreUObject.Object' and o['size']==40 and not o['fields']:
            for name,ctype,offset,size in [('VTable','std::uintptr_t*',0,8),('ObjectFlags','std::uint32_t',8,4),
                     ('InternalIndex','std::int32_t',12,4),('ClassPrivate','UClass*',16,8),
                     ('NamePrivate','FName',24,8),('OuterPrivate','UObject*',32,8)]:
                result.append(dict(name=name,cpp_name=name,cpp_type=ctype,offset=offset,extent=size,
                                   element_size=size,array_dim=1,type='NativeCore',detail={},deps=set()))
            return result
        for f in o["fields"]:
            if parameters and not ((f.get("flags") or 0)&0x80): continue
            f=dict(f)
            if parameters:
                flags=f.get('flags') or 0
                if flags&0x400:f['role']='return'
                elif flags&2 and flags&0x08000000:f['role']='const_ref'
                elif flags&0x100:f['role']='inout_ref' if flags&0x08000000 else 'out'
                else:f['role']='in'
            if not f["extent"] or f["offset"] is None or f["offset"]<0: raise CodegenError(f"{o['path']}.{f['name']}: missing field extent")
            d=dict(f.get("detail") or {});d.setdefault("type",f['type']);d.setdefault("size",f['element_size'])
            if f.get("target"):d.setdefault("target",f['target'])
            if f.get("bool_mask") is not None:d.setdefault("bool_mask",f['bool_mask'])
            ctype,deps=self.type_name(d,f['element_size'],o['path'],f['name'])
            if f['array_dim']>1:ctype=f"TStaticArray<{ctype}, {f['array_dim']}>"
            f.update(cpp_type=ctype,cpp_name=_unique(identifier(f['name']),used),detail=d,deps=deps)
            result.append(f)
        return result

    def layout(self, o: dict, fields: list[dict], methods: list[str], parameters=False) -> str:
        symbol=self.names[o['path']];size=o.get('parameter_size') if parameters else o['size']
        if size is None: raise CodegenError(f"{o['path']}: no verified size")
        alignment_known=not parameters and bool(o.get('alignment'))
        alignment=o.get('alignment') or 1
        if not isinstance(alignment,int) or alignment not in (1,2,4,8,16,32,64):alignment=1
        if parameters: alignment=1  # Invoke uses an aligned local instance of this exact parameter byte layout.
        if size and size%alignment:alignment=1;self.issue(o['path'],"", "alignment did not divide size")
        base=self.by_path.get(o.get('super')) if not parameters else None
        if base and base['kind'] not in {'class','struct'}:base=None
        cursor=(base['size'] or 0) if base else 0
        if cursor>size:raise CodegenError(f"{o['path']}: superclass exceeds derived size")
        base_decl=f" : public {self.names[base['path']]}" if base and cursor else ""
        alignment_decl=f" alignas({alignment})" if alignment_known else ''
        lines=[f"// {o['path']} | size 0x{size:X}",f"struct{alignment_decl} {symbol}{base_decl} {{"]
        assertions=[];groups=defaultdict(list)
        for f in fields:groups[f['offset']].append(f)
        for offset,group in sorted(groups.items()):
            if offset<cursor:
                if base and offset<(base['size'] or 0):
                    # The C++ base subobject occupies its complete sizeof. Unreal
                    # can put reflected fields in superclass tail padding; retain
                    # real inheritance and expose these bytes through accessors.
                    for f in group:
                        self.issue(o['path'],f['name'],'superclass tail padding exposed through an offset accessor')
                        mask=f['detail'].get('bool_mask')
                        if f['type']=='BoolProperty' and mask in (1,2,4,8,16,32,64,128):
                            lines.append(f"    bool Get_{f['cpp_name']}() const {{ return (reinterpret_cast<const std::uint8_t*>(this)[0x{offset:X}] & 0x{mask:X}) != 0; }}")
                            lines.append(f"    void Set_{f['cpp_name']}(bool value) {{ auto& byte=reinterpret_cast<std::uint8_t*>(this)[0x{offset:X}]; byte=static_cast<std::uint8_t>((byte & ~0x{mask:X}) | (value ? 0x{mask:X} : 0)); }}")
                        else:
                            lines.append(f"    {f['cpp_type']}& Get_{f['cpp_name']}() {{ return *reinterpret_cast<{f['cpp_type']}*>(reinterpret_cast<std::byte*>(this)+0x{offset:X}); }}")
                            lines.append(f"    {f['cpp_type']} const& Get_{f['cpp_name']}() const {{ return *reinterpret_cast<{f['cpp_type']} const*>(reinterpret_cast<const std::byte*>(this)+0x{offset:X}); }}")
                    continue
                raise CodegenError(f"{o['path']}: overlapping field at 0x{offset:X}")
            if offset>cursor:lines.append(f"    std::byte Pad_{cursor:04X}[0x{offset-cursor:X}];")
            if offset+max(f['extent'] for f in group)>size:raise CodegenError(f"{o['path']}: field exceeds declared size")
            bitfields=all(f['type']=='BoolProperty' and f['extent']==1 and f['detail'].get('bool_mask') in (1,2,4,8,16,32,64,128) and f['detail'].get('byte_offset',0)==0 for f in group)
            if bitfields:
                masks=[f['detail']['bool_mask'] for f in group]
                if len(masks)!=len(set(masks)):raise CodegenError(f"{o['path']}: duplicate boolean masks")
                lines.extend(["    union {",f"        std::uint8_t Bits_{offset:04X};","        struct {"])
                bit=0
                for f in sorted(group,key=lambda f:f['detail']['bool_mask']):
                    target_bit=f['detail']['bool_mask'].bit_length()-1
                    if target_bit>bit:lines.append(f"            std::uint8_t : {target_bit-bit};")
                    lines.append(f"            std::uint8_t {f['cpp_name']} : 1; // mask 0x{f['detail']['bool_mask']:02X}")
                    bit=target_bit+1
                if bit<8:lines.append(f"            std::uint8_t : {8-bit};")
                lines.extend(["        };","    };"])
                assertions.append((f"Bits_{offset:04X}",offset));cursor=offset+1
            elif len(group)==1:
                f=group[0]
                lines.append(f"    {f['cpp_type']} {f['cpp_name']}; // 0x{offset:X} (0x{f['extent']:X})")
                assertions.append((f['cpp_name'],offset));cursor=offset+f['extent']
            else:
                # Unknown masks cannot be represented as independent overlapping bools.
                extent=max(f['extent'] for f in group)
                lines.append(f"    std::byte UnresolvedOverlap_{offset:04X}[0x{extent:X}];")
                for f in group:self.issue(o['path'],f['name'],"overlapping members require recovered bool masks")
                assertions.append((f"UnresolvedOverlap_{offset:04X}",offset));cursor=offset+extent
        if cursor<size:lines.append(f"    std::byte Pad_{cursor:04X}[0x{size-cursor:X}];")
        lines.extend(methods);lines.append("};")
        if size:
            lines.append(f"static_assert(sizeof({symbol}) == 0x{size:X});")
            if alignment_known:lines.append(f"static_assert(alignof({symbol}) == {alignment});")
            for name,offset in assertions:lines.append(f"static_assert(offsetof({symbol}, {name}) == 0x{offset:X});")
        return '\n'.join(lines)+'\n'

    def signature(self, fn, fields):
        args=[];result='void';return_field=None
        for f in fields:
            flags=f.get('flags') or 0
            if flags&0x400:result=f['cpp_type'];return_field=f;continue
            reference=f['role'] in {'out','out_ref','inout_ref'}
            if reference:ctype=f['cpp_type']+'&'
            elif f['cpp_type'] in {'bool','float','double'} or f['cpp_type'].startswith('std::') or f['cpp_type'].endswith('*'):ctype=f['cpp_type']
            else:ctype='const '+f['cpp_type']+'&'
            args.append(f"{ctype} arg_{f['cpp_name']}")
        return result,', '.join(args),return_field

    def generate(self):
        decl=[];enums=[];parameter_bodies=[];bodies={};definitions=[];package_entries=defaultdict(list)
        for o in self.owners:
            symbol=self.names[o['path']]
            if o['kind']=='enum':
                n=self.enum_sizes[o['path']];signed=any(v['value']<0 for v in o['enum_values'])
                underlying=f"std::{'int' if signed else 'uint'}{n*8}_t"
                decl.append(f"enum class {symbol} : {underlying};")
                rows=[f"// {o['path']}",f"enum class {symbol} : {underlying} {{"];used=set()
                for v in o['enum_values']:
                    value=v['value'];literal=str(value) if value!=-(1<<63) else '(-9223372036854775807LL - 1)'
                    rows.append(f"    {_unique(identifier(v['name'].rsplit('::',1)[-1]),used)} = {literal},")
                rows.append('};');enums.append('\n'.join(rows));continue
            decl.append(f"struct {symbol};")
            if o['kind']=='function':
                fields=self.prepare_fields(o,True);self.fields[o['path']]=fields
                parameter_bodies.append(self.layout(o,fields,[],True))
                parent=o['path'].rsplit('.',1)[0]
                if parent in self.by_path and self.by_path[parent]['kind']=='class' and not o['name'].startswith('Default__'):
                    self.methods[parent].append(o)
            else:
                self.fields[o['path']]=self.prepare_fields(o)
                for f in self.fields[o['path']]:self.dependencies[o['path']].update(f['deps'])
                if o.get('super') in self.by_path:self.dependencies[o['path']].add(o['super'])
        # Layout definitions precede inline wrappers, breaking type/function cycles.
        for o in self.owners:
            if o['kind'] not in {'class','struct'}:continue
            symbol=self.names[o['path']];methods=[]
            if o['kind']=='class':
                methods.extend([f"    static UClass* StaticClass() {{ return static_cast<UClass*>(Runtime::FindObject({_cpp_string(o['path'])})); }}",
                                f"    static {symbol}* GetDefaultObject() {{ return static_cast<{symbol}*>(Runtime::GetDefaultObject({_cpp_string(o['path'])})); }}"])
                if o['path']=='/Script/CoreUObject.Object':methods.extend(["    std::string GetName() const { return Runtime::ObjectName(this); }","    std::string GetPathName() const { return Runtime::ObjectPath(this); }",
                     "    template<typename T=UObject> static T* FindObject(std::string_view path) { return static_cast<T*>(Runtime::FindObject(path)); }",
                     "    void ProcessEvent(UFunction* function, void* parameters) { Runtime::DispatchEvent(this, function, parameters); }"])
            used={f['cpp_name'] for f in self.fields[o['path']]}|{'StaticClass','GetDefaultObject',symbol,'GetName','GetPathName'}
            for fn in self.methods[o['path']]:
                fields=self.fields[fn['path']];ret,args,return_field=self.signature(fn,fields)
                method=_unique(identifier(fn['name']),used);static=bool((fn.get('flags') or 0)&0x2000)
                methods.append(f"    {'static ' if static else ''}{ret} {method}({args});")
                code=[f"inline {ret} {symbol}::{method}({args}) {{",f"    alignas(16) {self.names[fn['path']]} params{{}};"]
                for f in fields:
                    if f['role'] not in {'return','out'}:code.append(f"    params.{f['cpp_name']} = arg_{f['cpp_name']};")
                target='GetDefaultObject()' if static else 'this'
                code.append(f"    Runtime::Invoke({target}, {_cpp_string(fn['path'])}, {'&params' if fn.get('parameter_size') else 'nullptr'});")
                for f in fields:
                    if f['role'] in {'out','out_ref','inout_ref'}:code.append(f"    arg_{f['cpp_name']} = params.{f['cpp_name']};")
                if return_field:code.append(f"    return params.{return_field['cpp_name']};")
                code.append('}');definitions.append('\n'.join(code));self.wrapper_count+=1
            bodies[o['path']]=self.layout(o,self.fields[o['path']],methods)
        ordered=[];visiting=set();finished=set()
        def visit(path):
            if path in finished or path not in bodies:return
            if path in visiting:raise CodegenError(f"by-value type dependency cycle: {path}")
            visiting.add(path)
            for dependency in sorted(self.dependencies[path]):visit(dependency)
            visiting.remove(path);finished.add(path);ordered.append(path)
        for path in bodies:visit(path)
        for o in self.owners:
            if o['kind']=='function':continue
            package=o['path'].rsplit('.',1)[0].split('/')[-1]
            package_entries[identifier(package)].append(f"using {self.names[o['path']]} = ::SDK::{self.names[o['path']]};")
        files={'Basic.hpp':CORE,'Forward.hpp':'#pragma once\n#include "Basic.hpp"\nnamespace SDK {\n'+'\n'.join(decl)+'\n}\n',
               'Enums.hpp':'#pragma once\n#include "Forward.hpp"\nnamespace SDK {\n'+'\n'.join(enums)+'\n}\n',
               'Types.hpp':'#pragma once\n#include "Enums.hpp"\n#ifdef _MSC_VER\n#pragma warning(push)\n#pragma warning(disable:4201)\n#endif\n#pragma pack(push, 1)\nnamespace SDK {\n'+'\n'.join(bodies[p] for p in ordered)+'\n}\n#pragma pack(pop)\n#ifdef _MSC_VER\n#pragma warning(pop)\n#endif\n',
               'Parameters.hpp':'#pragma once\n#include "Types.hpp"\n#pragma pack(push, 1)\nnamespace SDK {\n'+'\n'.join(parameter_bodies)+'\n}\n#pragma pack(pop)\n',
               'Functions.hpp':'#pragma once\n#include "Parameters.hpp"\nnamespace SDK {\n'+'\n'.join(definitions)+'\n}\n',
               'SDK.hpp':'#pragma once\n#include "Functions.hpp"\n',
               'CompileCheck.cpp':'#include "SDK.hpp"\nint main() { return 0; }\n',
               'CMakeLists.txt':'cmake_minimum_required(VERSION 3.20)\nproject(EbonSDK LANGUAGES CXX)\nadd_library(EbonSDK STATIC Runtime.cpp)\ntarget_include_directories(EbonSDK PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})\ntarget_compile_features(EbonSDK PUBLIC cxx_std_17)\nif(MSVC)\n target_compile_options(EbonSDK PUBLIC /bigobj /utf-8)\nendif()\nadd_executable(EbonSDKCompileCheck CompileCheck.cpp)\ntarget_link_libraries(EbonSDKCompileCheck PRIVATE EbonSDK)\n'}
        for package,rows in package_entries.items():files[f'Packages/{package}.hpp']='#pragma once\n#include "../SDK.hpp"\nnamespace SDK::Packages::'+package+' {\n'+'\n'.join(rows)+'\n}\n'
        return files


def generate_cpp_sdk(dump_dir, output_dir, runtime_profile=None, network_capture=None):
    source=Path(dump_dir).resolve();output=Path(output_dir).resolve()
    if output==source or output in source.parents or source in output.parents:raise CodegenError('SDK output must be separate from source dump')
    if output.exists() and (not output.is_dir() or any(output.iterdir())):raise CodegenError('SDK output must be new or empty')
    state,owners,objects=_validate_dump(source)
    profile_path=Path(runtime_profile) if runtime_profile else Path(__file__).resolve().parents[2]/'profiles'/'nte-sdk-runtime.json'
    if profile_path.is_file():
        profile=json.loads(profile_path.read_text(encoding='utf-8'))
        if profile.get('module_sha256')==(state.get('build') or {}).get('sha256'):
            for table in state.get('runtime_vtables',[]):
                index=int(profile['process_event_vtable_index'])
                if table['object']=='/Script/CoreUObject.Default__Object' and (index>=len(table['entries']) or table['entries'][index]!=profile['process_event_rva']):
                    raise CodegenError('Captured ProcessEvent slot does not match the runtime profile')
            state['process_event_vtable_index']=int(profile['process_event_vtable_index'])
            state['process_event_rva']=profile['process_event_rva']
        elif runtime_profile:raise CodegenError('Runtime profile does not match the captured module SHA-256')
    generator=Generator(state,owners);files=generator.generate()
    files.update(runtime_files(state))
    network_source=Path(network_capture).resolve() if network_capture else source
    network_owners=owners
    network_state=state
    if network_capture:
        network_state=json.loads((network_source/'dump_state.json').read_text(encoding='utf-8'))
        if (network_state.get('build') or {}).get('sha256')!=(state.get('build') or {}).get('sha256'):
            raise CodegenError('Network snapshot module SHA-256 does not match the SDK source')
        network_owners=_jsonl(network_source/'types.jsonl')
    network_files,network_summary=generate_network_files(network_source,network_owners,verified_owners=owners if network_capture else None)
    files.update(network_files)
    files['SDK.hpp']+='#include "Network.hpp"\n'
    manifest={'format':2,'generator':'EbonCore-Dumper C++ SDK','source_build':state.get('build'),
              'source_objects':len(objects),'source_types':len(owners),'generated_layouts':sum(o['kind'] in {'class','struct'} for o in owners),
              'generated_parameter_structs':sum(o['kind']=='function' for o in owners),'generated_function_wrappers':generator.wrapper_count,
              'generated_enums':sum(o['kind']=='enum' for o in owners),
              'unresolved_fields':sum(i['reason']!='superclass tail padding exposed through an offset accessor' for i in generator.issues),
              'offset_accessors':sum(i['reason']=='superclass tail padding exposed through an offset accessor' for i in generator.issues),
              'alignment_unknown_layouts':sum(o['kind'] in {'class','struct'} and not o.get('alignment') for o in owners),
              'process_event_vtable_index':state.get('process_event_vtable_index'),
              'network':network_summary,
              'network_source':{'capture':str(network_source),'reflection_capture_success':bool(network_state.get('success')),
                                'reflection_errors':len(network_state.get('errors',[]))},
              'limitations':['Engine containers are non-owning views; allocator and mutation APIs are not synthesized.',
                             'Live calls require an initialized dispatcher and the game thread; compilation and mock dispatch do not prove live-call compatibility.']}
    files['sdk_manifest.json']=json.dumps(manifest,ensure_ascii=False,indent=2)+'\n'
    files['unresolved.json']=json.dumps(generator.issues,ensure_ascii=False,indent=2)+'\n'
    files['type_map.json']=json.dumps(generator.names,ensure_ascii=False,indent=2)+'\n'
    output.mkdir(parents=True)
    try:
        for relative,text in files.items():
            path=output/relative;path.parent.mkdir(parents=True,exist_ok=True);path.write_text(text,encoding='utf-8',newline='\n')
    except Exception:
        (output/'generation_failed.txt').write_text('Generation interrupted; do not consume this output.\n');raise
    return manifest


def runtime_files(state):
    return {'Runtime.cpp':RUNTIME.replace('@NAME_POOL@',str(int(state.get('name_pool_rva') or '0',0)))
             .replace('@OBJECT_ARRAY@',str(int(state.get('object_array_rva') or '0',0)))
             .replace('@PROCESS_EVENT@',str(state.get('process_event_vtable_index',-1))),
            'README.md':'''# Ebon C++ SDK

Include SDK.hpp and link Runtime.cpp (or the generated CMake library). Types.hpp contains real members, inheritance, padding, bitfields and static layout assertions. Parameters.hpp contains exact ProcessEvent parameter buffers; Functions.hpp supplies typed method wrappers with input/output marshalling. Packages/ headers provide package namespaces; type_map.json maps original reflection paths to C++ symbols.

Call SDK::Runtime::Initialize() inside the matching game process. If no ProcessEvent slot was verified, supply a verified slot or install dispatcher callbacks with SetCallbacks; unknown slots are never guessed. Call wrappers on the game thread. Static methods resolve the class default object. Native function addresses are not directly invoked with a guessed ABI.

TArray/FString and other engine containers are non-owning views. TMap/TSet preserve typed keys/elements and observed storage sizes; allocator internals, container mutation and ownership operations are not synthesized. Fields whose semantic type is still unknown use TUnresolved<N> and are listed in unresolved.json. Do not treat these as fully reconstructed types.
'''}


RUNTIME=r'''#include "Basic.hpp"
#include <unordered_map>
#include <mutex>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
namespace SDK::Runtime {
namespace {
std::uintptr_t Base=0;
std::int32_t EventIndex=@PROCESS_EVENT@;
FindObjectCallback FindCallback=nullptr;
DispatchCallback Dispatch=nullptr;
std::unordered_map<std::string,void*> Cache;
std::recursive_mutex Lock;
template<typename T> T Read(std::uintptr_t address) { T v{}; std::memcpy(&v,reinterpret_cast<const void*>(address),sizeof(v)); return v; }
std::string Name(FName value) {
    const auto pool=Base+@NAME_POOL@;
    const auto block=Read<std::uintptr_t>(pool+16+(value.ComparisonIndex>>16)*8);
    const auto address=block+(value.ComparisonIndex&65535)*2;
    const auto header=Read<std::uint16_t>(address);
    const auto length=header>>6;
    if(!length || length>1023) return {};
    std::string result;
    if(!(header&1)) result.assign(reinterpret_cast<const char*>(address+2),length);
    else {
#ifdef _WIN32
        const auto* text=reinterpret_cast<const wchar_t*>(address+2);
        const int count=WideCharToMultiByte(CP_UTF8,0,text,length,nullptr,0,nullptr,nullptr);
        result.resize(count); WideCharToMultiByte(CP_UTF8,0,text,length,result.data(),count,nullptr,nullptr);
#else
        throw std::runtime_error("Wide FName decoding requires Windows");
#endif
    }
    if(value.Number)result+="_"+std::to_string(value.Number-1);
    return result;
}
void* At(std::int32_t index) {
    const auto array=Base+@OBJECT_ARRAY@;
    const auto table=Read<std::uintptr_t>(array+16);
    const auto chunk=Read<std::uintptr_t>(table+(index/65536)*8);
    return Read<void*>(chunk+(index%65536)*24);
}
}
void SetCallbacks(FindObjectCallback findObject, DispatchCallback dispatch) {
    std::lock_guard<std::recursive_mutex> guard(Lock);FindCallback=findObject;Dispatch=dispatch;Cache.clear();
}
void Initialize(std::uintptr_t moduleBase,std::int32_t processEventIndex) {
    std::lock_guard<std::recursive_mutex> guard(Lock);
#ifdef _WIN32
    Base=moduleBase?moduleBase:reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
#else
    Base=moduleBase;
#endif
    if(!Base)throw std::runtime_error("SDK module base is not initialized");
    if(processEventIndex>=0)EventIndex=processEventIndex;
    Cache.clear();
}
std::string ObjectName(const void* object) {
    if(!Base||!object)throw std::runtime_error("SDK object/module is not initialized");
    return Name(Read<FName>(reinterpret_cast<std::uintptr_t>(object)+24));
}
std::string ObjectPath(const void* object) {
    if(!Base||!object)return {};
    std::string path;
    for(unsigned n=0;object&&n<64;++n){
        const auto name=ObjectName(object);if(name.empty())return {};
        path=path.empty()?name:name+"."+path;
        object=Read<void*>(reinterpret_cast<std::uintptr_t>(object)+32);
    }
    return object?std::string():path;
}
void RefreshObjectCache() {
    std::lock_guard<std::recursive_mutex> guard(Lock);
    if(!Base)throw std::runtime_error("Call SDK::Runtime::Initialize first");
    Cache.clear();const auto count=Read<std::int32_t>(Base+@OBJECT_ARRAY@+36);
    if(count<0||count>10000000)throw std::runtime_error("Object array header failed validation");
    for(std::int32_t i=0;i<count;++i){void* object=At(i);if(!object)continue;auto path=ObjectPath(object);if(!path.empty())Cache.emplace(std::move(path),object);}
}
void* FindObject(std::string_view path) {
    std::lock_guard<std::recursive_mutex> guard(Lock);
    if(FindCallback)return FindCallback(path);
    if(Cache.empty())RefreshObjectCache();
    auto it=Cache.find(std::string(path));if(it==Cache.end())throw std::runtime_error("Reflected object was not found: "+std::string(path));
    return it->second;
}
void* GetDefaultObject(std::string_view classPath) {
    auto path=std::string(classPath);const auto dot=path.rfind('.');
    if(dot==path.npos)throw std::runtime_error("Invalid reflected class path");
    path.insert(dot+1,"Default__");return FindObject(path);
}
void DispatchEvent(void* object,void* function,void* parameters) {
    if(!object)throw std::runtime_error("Cannot call a function on a null object");
    if(!function)throw std::runtime_error("Cannot call a null reflected function");
    if(Dispatch){Dispatch(object,function,parameters);return;}
    if(EventIndex<0)throw std::runtime_error("A verified ProcessEvent vtable index or dispatcher callback is required");
    auto vtable=*reinterpret_cast<std::uintptr_t**>(object);
    auto event=reinterpret_cast<DispatchCallback>(vtable[EventIndex]);
    event(object,function,parameters);
}
void Invoke(void* object,std::string_view path,void* parameters) { DispatchEvent(object,FindObject(path),parameters); }
}
'''
