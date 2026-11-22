#pragma once
#include "../sdk/ebon_reflection.hpp"
#include <atomic>
#include <array>
#include <memory>

namespace ebon::character_loader {

inline bool classPath(const std::string& path){
    if(path.size()>512||path.rfind("/Game/Blueprints/Character/Player/",0)!=0)return false;
    const auto dot=path.find('.');
    if(dot==std::string::npos||path.find('.',dot+1)!=std::string::npos||path.substr(path.size()-2)!="_C")return false;
    for(const unsigned char c:path)if(!std::isalnum(c)&&c!='/'&&c!='_'&&c!='.')return false;
    const auto slash=path.rfind('/',dot);
    return slash!=std::string::npos&&path.substr(slash+1,dot-slash-1)+"_C"==path.substr(dot+1);
}

// One request runs on the process's named game thread while it pumps its own
// visible Unreal window. Never infer a thread from its creation order.
class GameThreadQueue {
    enum class Phase { Pending,Running,Done,Cancelled };
    struct Job {
        DWORD thread=0;
        std::atomic<Phase> phase{Phase::Pending};
        std::function<void()> action;
        std::string error;
    };
    static inline std::shared_ptr<Job> active_;
    static LRESULT CALLBACK hook(int code,WPARAM message,LPARAM data)noexcept{
        const auto job=std::atomic_load(&active_);
        if(code>=0&&job&&GetCurrentThreadId()==job->thread){
            auto expected=Phase::Pending;
            if(job->phase.compare_exchange_strong(expected,Phase::Running)){
                try{job->action();}
                catch(const std::exception& error){job->error=error.what();}
                catch(...){job->error="unexpected character load exception";}
                job->phase.store(Phase::Done,std::memory_order_release);
            }
        }
        return CallNextHookEx(nullptr,code,message,data);
    }
    static BOOL CALLBACK window(HWND window,LPARAM context){
        DWORD pid=0;const auto thread=GetWindowThreadProcessId(window,&pid);
        if(pid!=GetCurrentProcessId()||!IsWindowVisible(window))return TRUE;
        wchar_t name[128]{};if(!GetClassNameW(window,name,128)||std::wstring(name)!=L"UnrealWindow")return TRUE;
        const auto handle=OpenThread(THREAD_QUERY_LIMITED_INFORMATION,FALSE,thread);if(!handle)return TRUE;
        PWSTR description=nullptr;const auto result=GetThreadDescription(handle,&description);CloseHandle(handle);
        const bool matches=SUCCEEDED(result)&&description&&std::wstring(description)==L"GameThread";
        if(description)LocalFree(description);
        if(!matches)return TRUE;
        auto& found=*reinterpret_cast<std::vector<HWND>*>(context);found.push_back(window);return TRUE;
    }
public:
    static bool execute(const std::function<void()>& action,DWORD timeout,std::string& error){
        std::vector<HWND> windows;EnumWindows(&window,reinterpret_cast<LPARAM>(&windows));
        if(windows.empty()){error="No visible UnrealWindow on a thread named GameThread; enter the game scene before requesting capture.";return false;}
        std::set<DWORD> threads;for(const auto handle:windows)threads.insert(GetWindowThreadProcessId(handle,nullptr));
        if(threads.size()!=1){error="Unreal window game-thread identity is ambiguous";return false;}
        auto job=std::make_shared<Job>();job->thread=*threads.begin();job->action=action;
        if(job->thread==GetCurrentThreadId()){error="Character dispatcher must be scheduled by its worker";return false;}
        std::shared_ptr<Job> empty;
        if(!std::atomic_compare_exchange_strong(&active_,&empty,job)){error="Another character load is already scheduled";return false;}
        HMODULE pinned=nullptr;
        if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,reinterpret_cast<LPCWSTR>(&hook),&pinned)){
            std::atomic_store(&active_,std::shared_ptr<Job>{});error="Cannot pin character dispatcher module";return false;
        }
        const auto handle=SetWindowsHookExW(WH_GETMESSAGE,&hook,pinned,job->thread);
        if(!handle){std::atomic_store(&active_,std::shared_ptr<Job>{});FreeLibrary(pinned);error="Cannot install game-thread message callback";return false;}
        const auto until=GetTickCount64()+timeout;
        if(!PostMessageW(windows.front(),WM_NULL,0,0)){auto phase=Phase::Pending;job->phase.compare_exchange_strong(phase,Phase::Cancelled);}
        while(job->phase.load(std::memory_order_acquire)!=Phase::Done&&job->phase.load()!=Phase::Cancelled){
            if(GetTickCount64()>=until){
                auto phase=Phase::Pending;
                job->phase.compare_exchange_strong(phase,Phase::Cancelled);
                // A synchronous engine load already in progress cannot be
                // safely interrupted. Its job and DLL stay alive until return.
            }
            Sleep(10);
        }
        UnhookWindowsHookEx(handle);std::atomic_store(&active_,std::shared_ptr<Job>{});FreeLibrary(pinned);
        if(job->phase.load()==Phase::Cancelled){error="Game-thread callback timed out before loading; no late load will run";return false;}
        error=job->error;return error.empty();
    }
};

class Loader {
    using Event=void(__fastcall*)(void*,void*,void*);
    struct String {const wchar_t* data;std::int32_t size,capacity;};
    struct MakeParameters {String path;std::array<unsigned char,32> result{};};
    struct LoadParameters {std::array<unsigned char,40> reference{};std::uint64_t result=0;};
    static_assert(sizeof(MakeParameters)==48&&sizeof(LoadParameters)==48);
    const MemoryReader* memory_=nullptr;
    Object receiver_,make_,load_;
    Event event_=nullptr;
    std::uint64_t makeNative_=0,loadNative_=0;
    Layout layout_;
    unsigned slot_=0;
    bool current(const Object& object)const{
        std::int32_t index=-1;std::uint64_t klass=0;
        return memory_->get(object.address+layout_.objIndex,index)&&index==object.index&&
            memory_->get(object.address+layout_.objClass,klass)&&klass==object.klass;
    }
    bool liveBinding()const{
        std::uint64_t table=0,event=0,make=0,load=0;
        return current(receiver_)&&current(make_)&&current(load_)&&
            memory_->get(receiver_.address,table)&&memory_->get(table+slot_*8,event)&&event==reinterpret_cast<std::uint64_t>(event_)&&
            memory_->get(make_.address+layout_.functionNative,make)&&make==makeNative_&&
            memory_->get(load_.address+layout_.functionNative,load)&&load==loadNative_;
    }
    static const Type* type(const Reflection& reflection,const std::string& path){
        for(const auto& type:reflection.types)if(type.path==path)return &type;return nullptr;
    }
    static bool field(const Type* type,const char* name,const char* kind,int offset,int size,const char* target=nullptr){
        if(!type)return false;for(const auto& p:type->fields)if(p.name==name)
            return p.type==kind&&p.offset==offset&&p.element==size&&p.array==1&&(!target||p.target==target);
        return false;
    }
    static bool prefix(const MemoryReader& memory,std::uint64_t address,const std::string& bytes){
        if(bytes.size()!=32||!memory.executable(address))return false;
        std::array<unsigned char,16> raw{};if(!memory.copy(address,raw.data(),raw.size()))return false;
        try{for(std::size_t i=0;i<raw.size();++i)if(raw[i]!=std::stoul(bytes.substr(i*2,2),nullptr,16))return false;}
        catch(const std::exception&){return false;}return true;
    }
public:
    explicit Loader(const Config& config):layout_(config){}
    bool bind(const MemoryReader& memory,const Reflection& reflection,const Config& config,const Module& module,std::string& error){
        const auto fail=[&](const char* message){memory_=nullptr;event_=nullptr;error=message;return false;};
        memory_=nullptr;event_=nullptr;
        if(!config.number("character_load_enabled",0))return fail("character loading is disabled in this profile");
        if(config.text("exe_sha256")!=reflection.moduleSha256)return fail("character loader build identity mismatch");
        const auto make=type(reflection,"/Script/Engine.KismetSystemLibrary.MakeSoftClassPath");
        const auto load=type(reflection,"/Script/Engine.KismetSystemLibrary.LoadClassAsset_Blocking");
        const auto soft=type(reflection,"/Script/CoreUObject.SoftObjectPath");
        const auto top=type(reflection,"/Script/CoreUObject.TopLevelAssetPath");
        if(!make||!load||!make->complete||!load->complete||make->parameterSize!=48||load->parameterSize!=48||
           make->fields.size()!=2||load->fields.size()!=2||!soft||!soft->complete||soft->size!=32||!top||!top->complete||top->size!=16||
           !field(make,"PathString","StrProperty",0,16)||!field(make,"ReturnValue","StructProperty",16,32,"/Script/CoreUObject.SoftClassPath")||
           !field(load,"AssetClass","SoftClassProperty",0,40)||!field(load,"ReturnValue","ClassProperty",40,8)||
           !field(soft,"AssetPath","StructProperty",0,16,top->path.c_str())||
           !field(soft,"SubPathString","Utf8StrProperty",16,16)||
           !field(top,"PackageName","NameProperty",0,8)||!field(top,"AssetName","NameProperty",8,8))
            return fail("character loader reflected parameter schema mismatch");
        const auto object=[&](const std::string& path,Object& result){
            for(const auto& o:reflection.objects)if(o.path==path){result=o;return true;}return false;
        };
        if(!object("/Script/Engine.Default__KismetSystemLibrary",receiver_)||!object(make->path,make_)||!object(load->path,load_))
            return fail("character loader function objects or Kismet CDO unavailable");
        const auto slot=config.number("process_event_vtable_index",-1);
        if(slot<0||slot>255)return fail("character loader ProcessEvent slot is unverified");
        slot_=static_cast<unsigned>(slot);std::uint64_t event=0,table=0;
        try{
            const auto entry=[&](const char* key){const auto rva=std::stoull(config.text(key),nullptr,0);
                if(rva>=module.size)throw std::runtime_error("loader RVA outside module");return module.base+rva;};
            makeNative_=entry("character_make_path_rva");loadNative_=entry("character_load_class_rva");event=entry("process_event_rva");
        }catch(const std::exception&){return fail("character loader native entry configuration invalid");}
        if(!prefix(memory,event,config.text("character_process_event_prefix"))||
           !prefix(memory,makeNative_,config.text("character_make_path_prefix"))||
           !prefix(memory,loadNative_,config.text("character_load_class_prefix")))return fail("character loader native code fingerprint mismatch");
        memory_=&memory;event_=reinterpret_cast<Event>(event);
        if(!memory.get(receiver_.address,table)||!liveBinding())return fail("character loader live function pointer mismatch");
        error.clear();return true;
    }
    bool load(const std::string& path,DWORD timeout,std::uint64_t& loaded,std::string& error){
        loaded=0;
        if(!classPath(path)){error="Only an exact player blueprint class path is accepted";return false;}
        if(!memory_||!event_){error="Character loader has not been validated";return false;}
        return GameThreadQueue::execute([&](){
            if(!liveBinding())throw std::runtime_error("Character loader binding changed before game-thread execution");
            const auto text=wide(path);MakeParameters make{{text.c_str(),static_cast<std::int32_t>(text.size()+1),static_cast<std::int32_t>(text.size()+1)}};
            event_(reinterpret_cast<void*>(receiver_.address),reinterpret_cast<void*>(make_.address),&make);
            std::uint64_t subpath=0;std::int32_t count=0;
            std::memcpy(&subpath,make.result.data()+16,8);std::memcpy(&count,make.result.data()+24,4);
            if(subpath||count)throw std::runtime_error("Unexpected subobject string returned for player class path");
            LoadParameters parameters;std::memcpy(parameters.reference.data()+8,make.result.data(),32);
            event_(reinterpret_cast<void*>(receiver_.address),reinterpret_cast<void*>(load_.address),&parameters);
            loaded=parameters.result;
            if(!loaded)throw std::runtime_error("Engine did not load the requested player class");
        },timeout,error);
    }
};
} // namespace ebon::character_loader
