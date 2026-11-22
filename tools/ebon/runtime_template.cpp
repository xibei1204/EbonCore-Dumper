// C++17, generated Runtime.cpp, Windows x64. Object pointers returned by lookup are borrowed.
#include "Basic.hpp"
#include <unordered_map>
#include <mutex>
#include <thread>
#include <vector>
#include <limits>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
namespace SDK::Runtime {
namespace {
std::uintptr_t Base=0;
std::int32_t EventIndex=@PROCESS_EVENT@;
std::uint64_t Epoch=1;
FindObjectCallback FindCallback=nullptr;
DispatchCallback Dispatch=nullptr;
ReadMemoryCallback Reader=nullptr;
AcquireLifetimeCallback Acquire=nullptr;
ReleaseLifetimeCallback Release=nullptr;
std::thread::id GameThread;
std::unordered_map<std::string,ObjectHandle> Cache;
std::recursive_mutex Lock;
thread_local unsigned LifetimeDepth=0;

bool Copy(std::uintptr_t address,void* output,std::size_t size) {
    if(!output||!size||address<0x10000||size>std::numeric_limits<std::uintptr_t>::max()-address)return false;
    if(Reader)return Reader(address,output,size);
#ifdef _WIN32
    for(auto at=address;at<address+size;){
        MEMORY_BASIC_INFORMATION region{};
        if(!VirtualQuery(reinterpret_cast<void*>(at),&region,sizeof(region))||region.State!=MEM_COMMIT||
           (region.Protect&(PAGE_NOACCESS|PAGE_GUARD)))return false;
        const auto next=reinterpret_cast<std::uintptr_t>(region.BaseAddress)+region.RegionSize;
        if(next<=at)return false;
        at=next;
    }
    __try { std::memcpy(output,reinterpret_cast<const void*>(address),size);return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
#else
    return false;
#endif
}
template<typename T> bool Read(std::uintptr_t address,T& value) { return Copy(address,&value,sizeof(value)); }
bool Executable(std::uintptr_t address) {
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION r{};
    if(!VirtualQuery(reinterpret_cast<void*>(address),&r,sizeof(r))||r.State!=MEM_COMMIT||
       (r.Protect&(PAGE_NOACCESS|PAGE_GUARD)))return false;
    const auto p=r.Protect&0xff;
    return p==PAGE_EXECUTE||p==PAGE_EXECUTE_READ||p==PAGE_EXECUTE_READWRITE||p==PAGE_EXECUTE_WRITECOPY;
#else
    (void)address;return false;
#endif
}
struct LifetimeScope {
    bool Held=false;
    LifetimeScope() {
        if(LifetimeDepth)return;
        if(!Acquire||!Release)throw std::runtime_error("Install an engine GC/lifetime guard before native dispatch");
        Held=Acquire();
        if(!Held)throw std::runtime_error("Engine lifetime guard is unavailable (GC or scene transition)");
        ++LifetimeDepth;
    }
    ~LifetimeScope(){if(Held){--LifetimeDepth;Release();}}
    LifetimeScope(const LifetimeScope&)=delete;
};
void CheckThread() {
    if(GameThread!=std::this_thread::get_id())throw std::runtime_error("SDK calls must run on the initialized game thread");
}
bool Slot(std::int32_t index,std::uintptr_t& item,std::uintptr_t& object,std::int32_t& serial) {
    std::uintptr_t table=0,chunk=0;std::int32_t count=0,chunks=0;std::uint32_t flags=0;
    const auto array=Base+@OBJECT_ARRAY@;
    if(!Base||index<0||!Read(array+@ARRAY_COUNT@,count)||count<0||count>10000000||index>=count||
       !Read(array+@ARRAY_CHUNKS@,table)||!table||!Read(array+@ARRAY_NUM_CHUNKS@,chunks)||
       chunks<=0||chunks>16384||index/@ELEMENTS_PER_CHUNK@>=chunks||
       !Read(table+(index/@ELEMENTS_PER_CHUNK@)*8,chunk)||!chunk)return false;
    item=chunk+(index%@ELEMENTS_PER_CHUNK@)*@ITEM_STRIDE@;
    if(!Read(item+@ITEM_OBJECT@,object)||!object||!Read(item+@ITEM_SERIAL@,serial)||serial<0||
       !Read(item+@ITEM_FLAGS@,flags)||(flags&@INVALID_INTERNAL_FLAGS@))return false;
    return true;
}
bool Capture(const void* pointer,ObjectHandle& handle) {
    const auto address=reinterpret_cast<std::uintptr_t>(pointer);
    std::int32_t index=-1,serial=0;std::uintptr_t item=0,object=0;std::uint32_t flags=0;
    if(!Base||!pointer||!Read(address+@OBJECT_INDEX@,index)||!Slot(index,item,object,serial)||object!=address||
       !Read(address+@OBJECT_FLAGS@,flags)||(flags&0x0001c000U))return false;
    ObjectHandle value{};value.Address=address;value.Index=index;value.Serial=serial;value.Epoch=Epoch;
    if(!Read(address,value.VTable)||!value.VTable||!Read(address+@OBJECT_CLASS@,value.Class)||!value.Class||
       !Read(address+@OBJECT_NAME@,value.Name)||!Read(address+@OBJECT_OUTER@,value.Outer))return false;
    std::uintptr_t after=0;std::int32_t afterSerial=0;
    if(!Slot(index,item,after,afterSerial)||after!=object||afterSerial!=serial)return false;
    handle=value;return true;
}
bool Same(const ObjectHandle& a,const ObjectHandle& b) {
    return a.Address==b.Address&&a.Index==b.Index&&a.Serial==b.Serial&&a.Epoch==b.Epoch&&
           a.Class==b.Class&&a.Outer==b.Outer&&a.VTable==b.VTable&&
           a.Name.ComparisonIndex==b.Name.ComparisonIndex&&a.Name.Number==b.Name.Number;
}
bool Valid(const ObjectHandle& handle,bool durable=true) {
    // Zero serials have not been assigned a weak identity by UE. Never pretend they are durable handles.
    if(handle.Epoch!=Epoch||(durable&&handle.Serial<=0))return false;
    ObjectHandle current{};
    return Capture(reinterpret_cast<void*>(handle.Address),current)&&Same(handle,current);
}
std::string Name(FName value) {
    const auto pool=Base+@NAME_POOL@;
    std::uint32_t current=0,cursor=0;std::uintptr_t block=0;std::uint16_t header=0;
    const auto index=value.ComparisonIndex>>@NAME_BITS@;
    const auto offset=(value.ComparisonIndex&((1U<<@NAME_BITS@)-1))*@NAME_STRIDE@;
    if(!Read(pool+@POOL_BLOCK@,current)||current>=8192||index>current||!Read(pool+@POOL_CURSOR@,cursor)||
       !Read(pool+@POOL_BLOCKS@+index*8,block)||!block||!Read(block+offset,header))return {};
    const auto length=header>>@NAME_SHIFT@;const bool wide=(header&@NAME_WIDE@)!=0;
    const auto bytes=static_cast<std::size_t>(length)*(wide?2:1);
    if(!length||length>1023||offset+@NAME_HEADER@+bytes>((1U<<@NAME_BITS@)*@NAME_STRIDE@)||
       (index==current&&offset+@NAME_HEADER@+bytes>cursor))return {};
    std::vector<char> data(bytes);
    if(!Copy(block+offset+@NAME_HEADER@,data.data(),bytes))return {};
    std::string result;
    if(!wide)result.assign(data.begin(),data.end());
    else {
#ifdef _WIN32
        std::wstring text(length,L'\0');std::memcpy(text.data(),data.data(),bytes);
        const int count=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),length,nullptr,0,nullptr,nullptr);
        if(count<=0)return {};
        result.resize(count);
        if(!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),length,result.data(),count,nullptr,nullptr))return {};
#else
        return {};
#endif
    }
    for(unsigned char ch:result)if(ch<32||ch==127)return {};
    if(value.Number)result+="_"+std::to_string(value.Number-1);
    return result;
}
std::string Path(const void* object) {
    std::string path;std::vector<ObjectHandle> chain;
    for(unsigned n=0;object&&n<64;++n){
        ObjectHandle value{};
        if(!Capture(object,value))return {};
        for(const auto& previous:chain)if(previous.Address==value.Address)return {};
        const auto name=Name(value.Name);if(name.empty())return {};
        chain.push_back(value);path=path.empty()?name:name+"."+path;
        object=reinterpret_cast<void*>(value.Outer);
    }
    if(object)return {};
    for(const auto& value:chain)if(!Valid(value,false))return {};
    return path;
}
void Reset() {Cache.clear();++Epoch;}
}
void SetCallbacks(FindObjectCallback findObject,DispatchCallback dispatch) {
    std::lock_guard<std::recursive_mutex> guard(Lock);
    FindCallback=findObject;Dispatch=dispatch;
    if(!Base)GameThread=std::this_thread::get_id();
    Reset();
}
void SetMemoryReader(ReadMemoryCallback reader) {
    std::lock_guard<std::recursive_mutex> guard(Lock);Reader=reader;Reset();
}
void SetLifetimeGuard(AcquireLifetimeCallback acquire,ReleaseLifetimeCallback release) {
    std::lock_guard<std::recursive_mutex> guard(Lock);
    if(bool(acquire)!=bool(release))throw std::runtime_error("Lifetime guard requires both acquire and release");
    Acquire=acquire;Release=release;Reset();
}
void Initialize(std::uintptr_t moduleBase,std::int32_t processEventIndex) {
    std::lock_guard<std::recursive_mutex> guard(Lock);
#ifdef _WIN32
    Base=moduleBase?moduleBase:reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
#else
    Base=moduleBase;
#endif
    if(!Base)throw std::runtime_error("SDK module base is not initialized");
    if(processEventIndex>=0){if(processEventIndex>=4096)throw std::runtime_error("ProcessEvent index is out of bounds");EventIndex=processEventIndex;}
    GameThread=std::this_thread::get_id();Reset();
}
ObjectHandle MakeObjectHandle(const void* object) {
    std::lock_guard<std::recursive_mutex> guard(Lock);ObjectHandle value{};
    if(!Capture(object,value)||value.Serial<=0)throw std::runtime_error("Object has no valid, assigned weak identity");
    return value;
}
void* ResolveObjectHandle(const ObjectHandle& handle) {
    std::lock_guard<std::recursive_mutex> guard(Lock);
    return Valid(handle)?reinterpret_cast<void*>(handle.Address):nullptr;
}
std::string ObjectName(const void* object) {
    std::lock_guard<std::recursive_mutex> guard(Lock);ObjectHandle value{};
    if(!Capture(object,value))throw std::runtime_error("SDK object is no longer alive");
    const auto result=Name(value.Name);
    return Valid(value,false)?result:std::string();
}
std::string ObjectPath(const void* object) {std::lock_guard<std::recursive_mutex> guard(Lock);return Path(object);}
void RefreshObjectCache() {
    std::lock_guard<std::recursive_mutex> guard(Lock);
    if(!Base)throw std::runtime_error("Call SDK::Runtime::Initialize first");
    std::int32_t count=0;
    if(!Read(Base+@OBJECT_ARRAY@+@ARRAY_COUNT@,count)||count<0||count>10000000)throw std::runtime_error("Object array header failed validation");
    std::unordered_map<std::string,ObjectHandle> fresh;
    for(std::int32_t i=0;i<count;++i){
        std::uintptr_t item=0,object=0;std::int32_t serial=0;ObjectHandle value{};
        if(!Slot(i,item,object,serial)||!Capture(reinterpret_cast<void*>(object),value))continue;
        auto path=Path(reinterpret_cast<void*>(object));
        if(!path.empty()&&Valid(value,false))fresh.emplace(std::move(path),value);
    }
    Cache.swap(fresh);
}
void* FindObject(std::string_view path) {
    std::lock_guard<std::recursive_mutex> guard(Lock);
    if(FindCallback)return FindCallback(path);
    const auto key=std::string(path);
    auto hit=Cache.find(key);
    if(hit!=Cache.end()&&(hit->second.Serial>0||LifetimeDepth)&&Valid(hit->second,false)&&Path(reinterpret_cast<void*>(hit->second.Address))==key)
        return reinterpret_cast<void*>(hit->second.Address);
    // Also retry misses: new scene objects can appear without changing the array count.
    RefreshObjectCache();hit=Cache.find(key);
    if(hit==Cache.end()||!Valid(hit->second,false))throw std::runtime_error("Reflected object was not found or expired: "+key);
    return reinterpret_cast<void*>(hit->second.Address);
}
ObjectHandle FindObjectHandle(std::string_view path) {return MakeObjectHandle(FindObject(path));}
void* GetDefaultObject(std::string_view classPath) {
    auto path=std::string(classPath);const auto dot=path.rfind('.');
    if(dot==path.npos)throw std::runtime_error("Invalid reflected class path");
    path.insert(dot+1,"Default__");return FindObject(path);
}
void DispatchEvent(void* object,void* function,void* parameters) {
    std::lock_guard<std::recursive_mutex> guard(Lock);CheckThread();
    if(!object||!function)throw std::runtime_error("Cannot call a null object or reflected function");
    // Callback hosts own the lifetime and dispatcher contract (including synthetic test hosts).
    if(Dispatch){Dispatch(object,function,parameters);return;}
    if(EventIndex<0)throw std::runtime_error("A verified ProcessEvent vtable index or dispatcher callback is required");
    LifetimeScope lifetime;
    ObjectHandle receiver{},target{};
    if(!Capture(object,receiver)||!Capture(function,target))throw std::runtime_error("Object or function expired before dispatch");
    std::uintptr_t event=0;
    if(!Read(receiver.VTable+static_cast<std::uintptr_t>(EventIndex)*8,event)||!Executable(event)||
       !Valid(receiver,false)||!Valid(target,false))throw std::runtime_error("ProcessEvent target failed lifetime validation");
    reinterpret_cast<DispatchCallback>(event)(object,function,parameters);
}
void Invoke(void* object,std::string_view path,void* parameters) {
    std::lock_guard<std::recursive_mutex> guard(Lock);CheckThread();
    if(Dispatch){DispatchEvent(object,FindObject(path),parameters);return;}
    LifetimeScope lifetime;
    DispatchEvent(object,FindObject(path),parameters);
}
void Invoke(const ObjectHandle& object,std::string_view path,void* parameters) {
    std::lock_guard<std::recursive_mutex> guard(Lock);CheckThread();
    LifetimeScope lifetime;
    auto* receiver=ResolveObjectHandle(object);
    if(!receiver)throw std::runtime_error("Object handle expired before dispatch");
    DispatchEvent(receiver,FindObject(path),parameters);
}
}
