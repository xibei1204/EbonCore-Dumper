import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from tools.ebon.cpp_sdk import generate_cpp_sdk, identifier
from tools.ebon.sdk_codegen import CodegenError


def cpp_fixture(root):
    def field(name, kind, offset, size, flags=0, target=None, **detail):
        return dict(kind='property', name=name, type=kind, offset=offset,
                    element_size=size, array_dim=1, size=size, flags=hex(flags),
                    target=target, bool_mask=detail.get('bool_mask'),
                    role='return' if flags & 0x400 else 'out_ref' if flags & 0x100 else 'in' if flags & 0x80 else 'member',
                    detail=dict(type=kind, size=size, target=target, **detail))
    obj='/Script/CoreUObject.Object';actor='/Script/Test.Actor';vector='/Script/Test.Vector'
    rows=[dict(kind='class',path=obj,name='Object',size=40,super=None,fields=[],enum_values=[]),
          dict(kind='struct',path=vector,name='Vector',size=12,super=None,alignment=4,
               fields=[field('X','FloatProperty',0,4),field('Y','FloatProperty',4,4),field('Z','FloatProperty',8,4)],enum_values=[]),
          dict(kind='class',path=actor,name='Actor',size=64,super=obj,fields=[
              field('Health','FloatProperty',40,4),
              field('bVisible','BoolProperty',44,1,bool_mask=1),field('bActive','BoolProperty',44,1,bool_mask=4),
              field('Position','StructProperty',48,12,target=vector)],enum_values=[]),
          dict(kind='class',path='/Script/Test.Child',name='Child',size=72,super=actor,
               fields=[field('TailValue','IntProperty',60,4),field('Count','IntProperty',64,4)],enum_values=[]),
          dict(kind='function',path=actor+'.Adjust',name='Adjust',size=32,parameter_size=16,super=None,flags='0x0',fields=[
              field('Amount','FloatProperty',0,4,0x80),field('OutValue','IntProperty',4,4,0x8000180),
              field('ReturnValue','IntProperty',8,4,0x480),field('bOkay','BoolProperty',12,1,0x80,bool_mask=255),
              field('OutByte','ByteProperty',13,1,0x180),
              field('Local','IntProperty',24,4)],enum_values=[]),
          dict(kind='function',path=actor+'.GetPosition',name='GetPosition',size=12,parameter_size=12,super=None,flags='0x2000',
               fields=[field('ReturnValue','StructProperty',0,12,0x480,target=vector)],enum_values=[]),
          dict(kind='enum',path='/Script/Test.State',name='State',size=None,super=None,fields=[],
               enum_values=[dict(name='State::NULL',value=0),dict(name='State::Large',value=256)]),
          dict(kind='struct',path='/Script/Test.Container',name='Container',size=16,super=None,
               fields=[field('Points','ArrayProperty',0,16,inner=dict(type='StructProperty',size=12,target=vector))],enum_values=[])]
    dump=root/'dump';dump.mkdir()
    state=dict(status='complete',success=True,layout_verified=True,objects=1,types=len(rows),errors=[],
               build={'sha256':'fixture'},name_pool_rva='0',object_array_rva='0')
    (dump/'dump_state.json').write_text(json.dumps(state))
    (dump/'types.jsonl').write_text(''.join(json.dumps(row)+'\n' for row in rows))
    (dump/'objects.jsonl').write_text('{}\n')
    return dump


class CppSdkTests(unittest.TestCase):
    def test_real_members_inheritance_params_and_no_guessed_dispatcher(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);output=root/'sdk';manifest=generate_cpp_sdk(cpp_fixture(root),output)
            types=(output/'Types.hpp').read_text()
            params=(output/'Parameters.hpp').read_text()
            functions=(output/'Functions.hpp').read_text()
            self.assertIn('struct UActor : public UObject',types)
            self.assertIn('float Health;',types)
            self.assertIn('FVector Position;',types)
            self.assertIn('std::uint8_t bVisible : 1',types)
            self.assertIn('TArray<FVector> Points;',types)
            self.assertIn('Get_TailValue()',types)
            self.assertNotIn(' Local;',params)
            self.assertIn('params.Amount = arg_Amount',functions)
            self.assertIn('arg_OutValue = params.OutValue',functions)
            self.assertIn('return params.ReturnValue',functions)
            self.assertEqual(manifest['generated_function_wrappers'],2)
            self.assertIn('A verified ProcessEvent', (output/'Runtime.cpp').read_text())

    def test_validates_runtime_profile_hash_and_escapes_macros(self):
        self.assertEqual(identifier('NULL'),'SDK_NULL')
        self.assertEqual(identifier('class'),'SDK_class')
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);dump=cpp_fixture(root);profile=root/'runtime.json'
            profile.write_text(json.dumps(dict(module_sha256='different',process_event_vtable_index=76,process_event_rva='0x1234')))
            with self.assertRaisesRegex(CodegenError,'SHA-256'):generate_cpp_sdk(dump,root/'sdk',profile)

    def test_injection_workflow_waits_and_emits_cpp_declarations(self):
        from tools.ebon_cli import inject_and_dump
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);exe=root/'inject.exe';dll=root/'dumper.dll'
            exe.write_bytes(b'fixture');dll.write_bytes(b'fixture')
            def capture(*args,**kwargs):
                cpp_fixture(root)
                return subprocess.CompletedProcess(args[0],0)
            with patch('tools.ebon_cli.subprocess.run',side_effect=capture):
                self.assertEqual(inject_and_dump(exe,dll,'fixture.exe',root/'profile.ini',root/'dump',root/'sdk'),0)
            self.assertIn('FVector Position;', (root/'sdk'/'Types.hpp').read_text())
            self.assertTrue((root/'sdk'/'Runtime.cpp').is_file())

    @unittest.skipUnless(os.name=='nt','MSVC Windows verification')
    def test_generated_layouts_and_wrappers_execute_with_mock_dispatch(self):
        setup=Path(os.environ['EBON_VCVARS']) if os.environ.get('EBON_VCVARS') else None
        if setup is not None and not setup.is_file():
            self.fail('EBON_VCVARS does not point to an existing toolchain script')
        if setup is None:
            vswhere=Path(os.environ.get('ProgramFiles(x86)',r'C:\Program Files (x86)'))/'Microsoft Visual Studio/Installer/vswhere.exe'
            if vswhere.is_file():
                found=subprocess.run([str(vswhere),'-latest','-prerelease','-products','*','-requires',
                                      'Microsoft.VisualStudio.Component.VC.Tools.x86.x64','-find',
                                      r'VC\Auxiliary\Build\vcvars64.bat'],capture_output=True,text=True,check=True,timeout=30)
                candidates=[Path(line) for line in found.stdout.splitlines() if line.strip()]
                setup=next((path for path in candidates if path.is_file()),None)
            if setup is None and not shutil.which('cl'):
                self.skipTest('MSVC build environment is unavailable')
        with tempfile.TemporaryDirectory(prefix='ebon-cpp-sdk-') as temporary:
            root=Path(temporary);output=root/'sdk';generate_cpp_sdk(cpp_fixture(root),output)
            (output/'Mock.cpp').write_text(r'''#include "SDK.hpp"
#include <cassert>
using namespace SDK;
static UActor Actor{};
static void* Find(std::string_view path) {
    if(path=="/Script/Test.Default__Actor")return &Actor;
    if(path=="/Script/Test.Actor.Adjust")return reinterpret_cast<void*>(1);
    if(path=="/Script/Test.Actor.GetPosition")return reinterpret_cast<void*>(2);
    return nullptr;
}
static void Event(void* object,void* function,void* buffer) {
    assert(object==&Actor);
    if(function==reinterpret_cast<void*>(1)){
        auto& p=*static_cast<Params_Adjust*>(buffer);
        assert(p.Amount==3.5f && p.OutValue==9 && p.bOkay && p.OutByte==0);
        p.OutValue=42;p.ReturnValue=77;p.OutByte=5;
    }else {auto& p=*static_cast<Params_GetPosition*>(buffer);p.ReturnValue={1,2,3};}
}
int main(){
    Runtime::SetCallbacks(Find,Event);
    std::int32_t out=9;std::uint8_t byte;assert(Actor.Adjust(3.5f,out,true,byte)==77 && out==42 && byte==5);
    const auto v=UActor::GetPosition();assert(v.X==1 && v.Z==3);
    Actor.bActive=1;Actor.bVisible=1;assert(Actor.Bits_002C==5);
    UChild child{};child.Get_TailValue()=123;
    assert(*reinterpret_cast<const std::int32_t*>(reinterpret_cast<const std::byte*>(&child)+60)==123);
    return 0;
}
''',encoding='utf-8')
            script=root/'verify.bat'
            toolchain=f'call "{setup}" >nul\nif errorlevel 1 exit /b 1\n' if setup else ''
            script.write_text(f'@echo off\n{toolchain}cl /nologo /std:c++17 /EHsc /utf-8 /W4 /WX Mock.cpp Runtime.cpp /Fe:Mock.exe\nif errorlevel 1 exit /b 1\nMock.exe\n',encoding='utf-8')
            result=subprocess.run(['cmd','/c',str(script)],cwd=output,capture_output=True,text=True,encoding="utf-8",errors="replace",timeout=60)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)


if __name__=='__main__':unittest.main()
