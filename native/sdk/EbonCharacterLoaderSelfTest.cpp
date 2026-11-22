#include "../dumper/ebon_character_loader.hpp"
#include <thread>
#include <iostream>

#pragma comment(lib,"user32.lib")
namespace {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
std::atomic<DWORD> gameThread{0},calledThread{0};
std::atomic<bool> paused{false},quit{false};
std::atomic<int> calls{0};
std::wstring observedPath;
std::uint64_t makeObject=0,loadObject=0;
void nativeMake(){}
void nativeLoad(){}
void __fastcall event(void*,void* function,void* parameters){
    calledThread=GetCurrentThreadId();++calls;
    auto bytes=static_cast<unsigned char*>(parameters);
    if(reinterpret_cast<std::uint64_t>(function)==makeObject){
        const wchar_t* text=nullptr;std::memcpy(&text,bytes,8);observedPath=text;
        const std::uint64_t names[]={123,456};std::memcpy(bytes+16,names,16);
    }else if(reinterpret_cast<std::uint64_t>(function)==loadObject){
        std::uint64_t weak=1,name=0;std::memcpy(&weak,bytes,8);std::memcpy(&name,bytes+8,8);
        require(!weak&&name==123,"soft class input did not preserve the validated path layout");
        const std::uint64_t result=0x12340000;std::memcpy(bytes+40,&result,8);
    }else throw std::runtime_error("unexpected reflected call");
}
struct GameWindow {
    HWND window=nullptr;std::thread thread;
    GameWindow(){
        thread=std::thread([&](){
            SetThreadDescription(GetCurrentThread(),L"GameThread");
            WNDCLASSW cls{};cls.lpfnWndProc=DefWindowProcW;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"UnrealWindow";
            RegisterClassW(&cls);
            window=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,cls.lpszClassName,L"Synthetic character loader",
                WS_POPUP|WS_VISIBLE,-32000,-32000,1,1,nullptr,nullptr,cls.hInstance,nullptr);
            gameThread=GetCurrentThreadId();MSG message{};
            while(!quit){
                if(!paused)while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
                Sleep(1);
            }
            DestroyWindow(window);UnregisterClassW(cls.lpszClassName,cls.hInstance);
        });
        const auto until=GetTickCount64()+5000;while(!gameThread&&GetTickCount64()<until)Sleep(1);
        require(gameThread&&window,"synthetic game window failed to initialize");
    }
    ~GameWindow(){quit=true;if(thread.joinable())thread.join();}
};
std::string prefix(std::uint64_t address){
    const auto bytes=reinterpret_cast<const unsigned char*>(address);static const char hex[]="0123456789abcdef";
    std::string result;for(unsigned i=0;i<16;++i){result+=hex[bytes[i]>>4];result+=hex[bytes[i]&15];}return result;
}
int run(){
    ebon::Config config;config.values["exe_sha256"]="synthetic-character-loader";
    config.values["character_load_enabled"]="1";config.values["process_event_vtable_index"]="76";
    const auto base=reinterpret_cast<std::uint64_t>(GetModuleHandleW(nullptr));
    ebon::Module module{base,0x10000000,L"synthetic",L"synthetic"};
    const std::vector<std::pair<std::string,std::uint64_t>> entries={
        {"character_make_path",reinterpret_cast<std::uint64_t>(&nativeMake)},
        {"character_load_class",reinterpret_cast<std::uint64_t>(&nativeLoad)},
        {"process_event",reinterpret_cast<std::uint64_t>(&event)}};
    for(const auto& entry:entries){config.values[entry.first+"_rva"]=ebon::hex(entry.second-base);
        config.values[(entry.first=="process_event"?"character_process_event":entry.first)+"_prefix"]=prefix(entry.second);}
    std::array<std::uint64_t,256> table{};table[76]=reinterpret_cast<std::uint64_t>(&event);
    std::array<std::uint64_t,40> receiver{},make{},load{};
    const auto initialize=[&](std::array<std::uint64_t,40>& bytes,int index){
        bytes[0]=reinterpret_cast<std::uint64_t>(table.data());bytes[2]=0x100000;
        std::memcpy(reinterpret_cast<unsigned char*>(bytes.data())+12,&index,4);
    };
    initialize(receiver,1);initialize(make,2);initialize(load,3);
    makeObject=reinterpret_cast<std::uint64_t>(make.data());loadObject=reinterpret_cast<std::uint64_t>(load.data());
    make[27]=reinterpret_cast<std::uint64_t>(&nativeMake);load[27]=reinterpret_cast<std::uint64_t>(&nativeLoad);
    ebon::InProcessReader memory;ebon::Reflection reflection(memory,config,module);reflection.moduleSha256=config.text("exe_sha256");
    const std::string owner="/Script/Engine.KismetSystemLibrary";
    const auto object=[&](int index,std::uint64_t address,const std::string& path){
        reflection.objects.push_back({index,address,0x100000,path.substr(path.rfind('.')+1),"synthetic",path});
    };
    object(1,reinterpret_cast<std::uint64_t>(receiver.data()),"/Script/Engine.Default__KismetSystemLibrary");
    object(2,makeObject,owner+".MakeSoftClassPath");object(3,loadObject,owner+".LoadClassAsset_Blocking");
    const auto property=[](const char* name,const char* type,int offset,int size,const char* target=""){
        ebon::Property value;value.name=name;value.type=type;value.offset=offset;value.element=size;value.array=1;value.target=target;return value;
    };
    ebon::Type maker;maker.path=owner+".MakeSoftClassPath";maker.parameterSize=std::uint16_t(48);
    maker.fields={property("PathString","StrProperty",0,16),property("ReturnValue","StructProperty",16,32,"/Script/CoreUObject.SoftClassPath")};
    ebon::Type loader;loader.path=owner+".LoadClassAsset_Blocking";loader.parameterSize=std::uint16_t(48);
    loader.fields={property("AssetClass","SoftClassProperty",0,40),property("ReturnValue","ClassProperty",40,8)};
    ebon::Type soft;soft.path="/Script/CoreUObject.SoftObjectPath";soft.size=32;
    soft.fields={property("AssetPath","StructProperty",0,16,"/Script/CoreUObject.TopLevelAssetPath"),property("SubPathString","Utf8StrProperty",16,16)};
    ebon::Type top;top.path="/Script/CoreUObject.TopLevelAssetPath";top.size=16;
    top.fields={property("PackageName","NameProperty",0,8),property("AssetName","NameProperty",8,8)};
    reflection.types={maker,loader,soft,top};
    ebon::character_loader::Loader capture(config);std::string error;
    require(capture.bind(memory,reflection,config,module,error),error.c_str());
    auto bad=config;bad.values["character_make_path_prefix"]=std::string(32,'0');
    ebon::character_loader::Loader invalid(bad);require(!invalid.bind(memory,reflection,bad,module,error),"changed function bytes were accepted");
    std::uint64_t rejected=0;
    require(!invalid.load("/Game/Blueprints/Character/Player/Test.Test_C",10,rejected,error)&&calls==0,
            "failed binding retained an executable loader");
    reflection.types[1].fields[1].offset=32;
    require(!invalid.bind(memory,reflection,config,module,error),"wrong return offset was accepted");
    reflection.types[1].fields[1].offset=40;
    require(!ebon::character_loader::classPath("/Game/Blueprints/Character/Player/../Bad.Bad_C"),"traversal class path accepted");
    GameWindow game;std::uint64_t result=0;const std::string target="/Game/Blueprints/Character/Player/Player_Test.Player_Test_C";
    require(capture.load(target,1000,result,error),error.c_str());
    require(result==0x12340000&&calls==2&&calledThread==gameThread&&observedPath==ebon::wide(target),"character calls did not run with the expected parameters on the game thread");
    paused=true;Sleep(20);std::atomic<bool> late{false};
    require(!ebon::character_loader::GameThreadQueue::execute([&](){late=true;},25,error),"paused game thread did not time out");
    paused=false;Sleep(50);require(!late,"a cancelled request executed after timeout");
    std::cout<<"Character loader self-test passed: game-thread dispatch, reflected parameters, fingerprint/schema rejection and timeout cancellation\n";
    return 0;
}
}
int main(){try{return run();}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
