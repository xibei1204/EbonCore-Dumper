// Exercises the actual DLL startup and file output in a disposable host process.
#include "../common/ebon_common.hpp"
#include <array>

alignas(8) static std::array<std::uint8_t,4096> globals{};
static void fixtureFunction() {}

template<class T> static void put(std::uint64_t address,T value) {
    std::memcpy(reinterpret_cast<void*>(address),&value,sizeof(value));
}

static void makeFixture(std::vector<std::uint8_t>& storage) {
    const auto base=reinterpret_cast<std::uint64_t>(storage.data());
    const auto g=reinterpret_cast<std::uint64_t>(globals.data());
    const auto pool=g+0x200,array=g+0x300,vtable=g+0x800;
    const auto block=base+0x10000,chunks=base+0x20000,items=base+0x21000;
    const auto fieldClass=base+0x30000,field=base+0x31000;
    const auto cls=base+0x40000,str=base+0x40100,fn=base+0x40200;
    const auto en=base+0x40300,example=base+0x40400,actor=base+0x40500;
    const std::vector<std::string> strings={"None","ByteProperty","IntProperty","BoolProperty","ScriptStruct","Class","Function","Enum","EExample","FExample","Actor","Health","Visible","Extra"};
    std::map<std::string,std::uint32_t> ids;
    std::uint32_t cursor=0;
    for(const auto& text:strings){
        ids[text]=cursor/2;
        put(block+cursor,(std::uint16_t)(text.size()<<6));
        std::memcpy(reinterpret_cast<void*>(block+cursor+2),text.data(),text.size());
        cursor+=(std::uint32_t)((2+text.size()+1)&~1u);
    }
    put(pool+16,block);put(pool+12,cursor);put(pool+8,std::uint32_t(0));
    put(array+16,chunks);put(chunks,items);put(array+36,std::int32_t(6));
    put(array+32,std::int32_t(65536));put(array+44,std::int32_t(1));put(array+40,std::int32_t(1));
    put(vtable,reinterpret_cast<std::uint64_t>(&fixtureFunction));
    struct Object {std::uint64_t address,klass;const char* name;};
    const Object objects[]={{cls,cls,"Class"},{str,cls,"ScriptStruct"},{fn,cls,"Function"},{en,cls,"Enum"},{example,en,"EExample"},{actor,str,"Actor"}};
    for(int i=0;i<6;++i){
        const auto p=objects[i].address;
        put(items+i*24,p);put(p,vtable);put(p+12,std::int32_t(i));
        put(p+16,objects[i].klass);put(p+24,ids[objects[i].name]);
        put(p+88,std::int32_t(16));
    }
    put(actor+80,field);
    put(fieldClass,ids["IntProperty"]);put(field+8,fieldClass);
    put(field+40,ids["Health"]);put(field+56,std::int32_t(1));
    put(field+60,std::int32_t(4));put(field+76,std::int32_t(0));
}

int wmain(int argc,wchar_t** argv) {
    if(argc!=4)return 2;
    const bool missing=std::wstring(argv[1])==L"missing";
    const bool delayed=std::wstring(argv[1])==L"delayed";
    const bool aes=std::wstring(argv[1])==L"aes";
    const bool aesError=std::wstring(argv[1])==L"aes_error";
    const bool aesHotkey=std::wstring(argv[1])==L"aes_hotkey";
    const bool resources=std::wstring(argv[1])==L"resources";
    const auto dll=std::filesystem::absolute(argv[2]);
    const auto output=std::filesystem::absolute(argv[3]);
    const auto profile=dll.parent_path()/"fixture.ini";
    const auto log=dll.parent_path()/"fixture.log";
    wchar_t consoleTitle[1024]{};
    GetConsoleTitleW(consoleTitle,1024);
    std::vector<std::uint8_t> storage(0x80000);
    if(!missing)makeFixture(storage);
    if(delayed)put(reinterpret_cast<std::uint64_t>(storage.data())+0x40500+80,std::uint64_t(0));
    wchar_t self[32768]{};
    GetModuleFileNameW(nullptr,self,32768);
    const auto module=std::filesystem::path(self).filename().u8string();
    const auto pak=aesError?dll.parent_path()/"fixture.pak":dll.parent_path()/"paks"/"fixture.pak";
    // aes_hotkey=1 leaves the scan disarmed until the hotkey request; every other
    // mode keeps the immediate start so the reflection timing assertions hold.
    ebon::save(profile,std::string("aes_hunt=")+(aes||aesError||aesHotkey?"1":"0")+
               "\naes_hotkey="+(aesHotkey?"1":"0")+
               "\naes_timeout_seconds=5\naes_retry_ms=25\naes_pak="+
               pak.u8string()+"\nmodule="+module+"\nclient_root="+(dll.parent_path()/"client").u8string()+"\n");
    ebon::save(dll.parent_path()/"EbonDumper.ini",
               "auto_dump=1\nconsole=0\nsettle_seconds=0\nmax_wait_seconds=2\nprofile="+profile.u8string()+
               "\noutput="+output.u8string()+"\nlog="+log.u8string()+"\n");
    SetEnvironmentVariableW(L"EBON_AUTO_DUMP",nullptr);
    const auto start=GetTickCount64();
    if(!LoadLibraryW(dll.c_str())){std::cerr<<"LoadLibrary failed="<<GetLastError()<<"\n";return 3;}
    bool sawProgress=false;
    ULONGLONG firstProgress=0;
    bool independentAes=false;
    bool hotkeyRequested=false,hotkeyStarted=false;
    unsigned resourceRequests=0;
    while(GetTickCount64()-start<10000){
        // The request file is the terminal-free equivalent of the F9 press.
        if(aesHotkey&&!hotkeyRequested&&std::filesystem::is_directory(output)){
            ebon::save(output/"aes_hunt.request","");hotkeyRequested=true;
        }
        std::ifstream input(output/"dump_state.json",std::ios::binary);
        const std::string state((std::istreambuf_iterator<char>(input)),{});
        input.close();
        if(state.find("\"success\":false")!=state.npos){sawProgress=true;if(!firstProgress)firstProgress=GetTickCount64();}
        std::ifstream logs(log,std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(logs)),{});
        logs.close();
        if(delayed&&text.find("metadata is not ready")!=text.npos)
            put(reinterpret_cast<std::uint64_t>(storage.data())+0x40500+80,reinterpret_cast<std::uint64_t>(storage.data())+0x31000);
        if(missing&&state.find("\"status\":\"failed\"")!=state.npos&&text.find("capture failed;")!=text.npos){
            const bool ok=sawProgress&&state.find("found 0")!=state.npos&&
                !std::filesystem::exists(output/"objects.jsonl")&&GetTickCount64()-firstProgress<6000;
            std::cout<<"DLL missing-layout/timeout: "<<(ok?"passed":"failed")<<" elapsed_ms="<<GetTickCount64()-start<<"\n";
            return ok?0:4;
        }
        if(!missing&&state.find("\"success\":true")!=state.npos&&text.find("dump finished:")!=text.npos){
            if(resources){
                if(!resourceRequests){ebon::save(output/"resource_capture.request","");resourceRequests=1;}
                std::ifstream resourceFile(output/"resources"/("res_"+std::to_string(resourceRequests))/"resource_state.json",std::ios::binary);
                const std::string resourceState((std::istreambuf_iterator<char>(resourceFile)),{});
                resourceFile.close();
                if(resourceState.find("\"status\":\"no_resources\"")==resourceState.npos){Sleep(10);continue;}
                if(resourceRequests==1){ebon::save(output/"resource_capture.request","");resourceRequests=2;Sleep(10);continue;}
            }
            if(aes||aesError||aesHotkey){
                std::ifstream aesFile(output/"aes"/"aes_state.json",std::ios::binary);
                const std::string aesState((std::istreambuf_iterator<char>(aesFile)),{});
                aesFile.close();
                if(aesError){
                    if(aesState.find("\"status\":\"error\"")==aesState.npos){Sleep(10);continue;}
                }else if(aesHotkey){
                    // Nothing may start the scan except the hotkey listener itself.
                    if(aesState.find("\"status\":\"timed_out\"")==aesState.npos){Sleep(10);continue;}
                    hotkeyStarted=true;
                }else{
                    if(aesState.find("\"status\":\"scanning\"")!=aesState.npos)independentAes=true;
                    if(aesState.find("\"status\":\"timed_out\"")==aesState.npos){Sleep(10);continue;}
                    if(!independentAes){std::cerr<<"AES blocked reflection capture\n";return 7;}
                }
            }
            std::ifstream types(output/"types.jsonl",std::ios::binary);
            const std::string metadata((std::istreambuf_iterator<char>(types)),{});
            wchar_t currentTitle[1024]{};
            GetConsoleTitleW(currentTitle,1024);
            const bool ok=metadata.find("Health")!=metadata.npos&&state.find("\"objects\":6")!=state.npos&&
                          std::wstring(currentTitle)==consoleTitle;
            if(aesHotkey&&!hotkeyStarted){std::cerr<<"AES hotkey request did not start the scan\n";return 8;}
            std::cout<<"DLL automatic discovery/collection: "<<(ok?"passed":"failed")<<" elapsed_ms="<<GetTickCount64()-start<<"\n";
            return ok?0:5;
        }
        Sleep(10);
    }
    std::cerr<<"DLL test did not produce the expected terminal state\n";
    return 6;
}
