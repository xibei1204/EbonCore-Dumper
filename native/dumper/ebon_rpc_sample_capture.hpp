#pragma once
#include "../sdk/ebon_reflection.hpp"
#include "../common/ebon_console.hpp"
#include "ebon_sync_sample_capture.hpp"
#include <atomic>
#include <deque>
#include <mutex>

namespace ebon::rpc_samples {
struct ArrayHeader {std::uint64_t data=0;std::int32_t count=0,capacity=0;};
struct RpcLayout {std::int32_t size=0,messageId=-1,parameters=-1,wrappers=-1,readIndex=-1,wrapperSize=0,typeInfo=-1,type=-1,customName=-1,binaryData=-1;};
struct FogLayout {std::int32_t size=0,name=-1,change=-1,row=-1,oldRow=-1,oldRowSize=0;};
struct NativeFrameLayout {std::int32_t node=-1,object=-1,code=-1,locals=-1,currentFunction=-1;};

inline bool nativeFrameParameters(const MemoryReader& memory,std::uint64_t frame,const NativeFrameLayout& layout,
                                  std::uint64_t object,std::uint64_t function,std::uint64_t& parameters){
    parameters=0;
    if(frame<0x10000||frame>UINT64_MAX-520)return false;
    for(auto offset:{layout.node,layout.object,layout.code,layout.locals,layout.currentFunction})
        if(offset<0||offset>512||(offset&7))return false;
    std::uint64_t node=0,owner=0,code=0,current=0,locals=0;
    if(!memory.get(frame+layout.node,node)||node!=function||!memory.get(frame+layout.object,owner)||owner!=object||
       !memory.get(frame+layout.code,code)||code||!memory.get(frame+layout.currentFunction,current)||current!=function||
       !memory.get(frame+layout.locals,locals)||locals<0x10000)return false;
    parameters=locals;return true;
}

inline bool wrapperArrayLayoutMatches(const Property* property,std::int32_t wrapperSize){
    if(!property||wrapperSize<=0||property->type!="ArrayProperty"||property->element<16)return false;
    const auto inner="\"inner\":{\"type\":\"StructProperty\",\"size\":"+std::to_string(wrapperSize)+",";
    return property->detail.find(inner)!=std::string::npos&&
        property->detail.find("\"target\":\"/Script/HottaFramework.ParameterWrapper\"")!=std::string::npos;
}

// HottaRpcMessage inherits its wrapper array at offset zero. GameDataRPC has
// a separate message ID before InParameters and therefore uses another layout.
inline bool friendRpcLayout(const Type* rpc,const Type* message,const Type* params,
                            const Type* wrapper,const Type* info,RpcLayout& result,bool targetActor=false){
    result={};
    const auto field=[](const Type* t,const char* name)->const Property*{
        if(t)for(const auto& f:t->fields)if(f.name==name)return &f;return nullptr;
    };
    const auto matches=[](const Property* f,const char* type,int offset,int size){
        return f&&f->type==type&&f->offset==offset&&f->element==size&&f->array==1;
    };
    if(!rpc||!message||!params||!wrapper||!info||!rpc->complete||!message->complete||!params->complete||
       !wrapper->complete||!info->complete||rpc->kind!="function"||rpc->size!=(targetActor?40:32)||rpc->parameterSize!=(targetActor?40:32)||
       message->path!="/Script/HottaFramework.HottaRpcMessage"||message->size!=32||
       params->path!="/Script/HottaFramework.ParameterWrapperArray"||params->size!=24||message->super!=params->path||
       wrapper->path!="/Script/HottaFramework.ParameterWrapper"||wrapper->size!=32||
       info->path!="/Script/HottaFramework.ParameterTypeInfo"||info->size!=12)return false;
    const auto in=field(rpc,"InMessage"),id=field(message,"Type"),array=field(params,"ParameterWrappers"),read=field(params,"ReadParameterIndex");
    const auto wrapperInfo=field(wrapper,"TypeInfo"),binary=field(wrapper,"BinaryData"),kind=field(info,"Type"),name=field(info,"CustomTypeName");
    const auto actor=field(rpc,"InTargetActor");
    if(targetActor&&(!matches(actor,"ObjectProperty",32,8)||actor->target!="/Script/Engine.Actor"))return false;
    if(!matches(in,"StructProperty",0,32)||in->target!=message->path||rpc->fields.size()!=(targetActor?2u:1u)||
       !matches(id,"IntProperty",24,4)||!matches(array,"ArrayProperty",0,16)||!wrapperArrayLayoutMatches(array,32)||
       !matches(read,"IntProperty",16,4)||!matches(wrapperInfo,"StructProperty",0,12)||wrapperInfo->target!=info->path||
       !matches(binary,"ArrayProperty",16,16)||binary->detail.find("\"type\":\"ByteProperty\"")==std::string::npos||
       !matches(kind,"EnumProperty",0,1)||!matches(name,"NameProperty",4,8))return false;
    result={targetActor?40:32,id->offset,in->offset,array->offset,read->offset,32,wrapperInfo->offset,kind->offset,name->offset,binary->offset};
    return true;
}

inline bool captureFogRecord(const MemoryReader& memory,std::uint64_t base,const FogLayout& layout,
                             std::uint64_t object,std::uint64_t function,std::uint64_t sampleId,
                             std::uint64_t tickMs,std::string& output,const char* stage="ProcessEvent_parameters"){
    if(base<0x10000||layout.size<=0||layout.size>4096||layout.name<0||layout.name+16>layout.size||
       layout.change<0||layout.change+4>layout.size||layout.row<0||layout.row+4>layout.size||
       layout.oldRow<0||layout.oldRowSize<=0||layout.oldRow+layout.oldRowSize>layout.size)return false;
    ArrayHeader nameArray;std::int32_t change=0,rowIndex=0;
    if(!memory.get(base+layout.name,nameArray)||nameArray.count<1||nameArray.count>4096||nameArray.capacity<nameArray.count||
       nameArray.capacity>16384||nameArray.data<0x10000||(nameArray.data&1)||
       !memory.get(base+layout.change,change)||!memory.get(base+layout.row,rowIndex))return false;
    std::wstring wideName(nameArray.count,L'\0');
    if(!memory.copy(nameArray.data,wideName.data(),wideName.size()*sizeof(wchar_t))||wideName.back()!=L'\0')return false;
    wideName.pop_back();if(wideName.find(L'\0')!=wideName.npos)return false;
    std::string name;try{name=utf8(wideName);}catch(...){return false;}
    std::vector<std::uint8_t> raw((std::size_t)layout.size);if(!memory.copy(base,raw.data(),raw.size()))return false;
    const auto hex=[](const std::uint8_t* p,std::size_t n){static const char d[]="0123456789abcdef";std::string s;s.reserve(n*2);for(std::size_t i=0;i<n;++i){s+=d[p[i]>>4];s+=d[p[i]&15];}return s;};
    output="{\"kind\":\"OnRecChange_MiniMapFogRec\",\"capture_stage\":"+quote(stage)+",\"wire_packet_bytes_captured\":false,\"sample_id\":"+std::to_string(sampleId)+
        ",\"tick_ms\":"+std::to_string(tickMs)+",\"record_name\":"+quote(name)+
        ",\"change_type\":"+std::to_string(change)+",\"row_index\":"+std::to_string(rowIndex)+
        ",\"object_address\":"+quote(ebon::hex(object))+",\"function_address\":"+quote(ebon::hex(function))+
        ",\"params_hex\":"+quote(hex(raw.data(),raw.size()))+",\"old_row_hex\":"+quote(hex(raw.data()+layout.oldRow,(std::size_t)layout.oldRowSize))+"}";
    return true;
}

inline bool captureClientGameDataRpc(const MemoryReader& memory,std::uint64_t base,const RpcLayout& layout,
                                     std::int32_t messageFilter,std::size_t maxPayload,
                                     const std::map<std::int64_t,std::string>& enumNames,
                                     std::uint64_t object,std::uint64_t function,std::uint64_t sampleId,
                                     std::uint64_t tickMs,std::string& output,const char* stage="ProcessEvent_parameters",
                                     const char* rpcKind="ClientGameDataRPC",const char* functionPath=nullptr,
                                     const char* direction=nullptr,const char* moduleSha256=nullptr,
                                     std::uint64_t namePool=0,const Layout* nameLayout=nullptr){
    if(base<0x10000||layout.size<32||layout.messageId<0||layout.parameters<0||layout.wrappers<0||layout.readIndex<0||
       layout.messageId+4>layout.size||layout.parameters+layout.wrappers+16>layout.size||layout.parameters+layout.readIndex+4>layout.size||
       layout.wrapperSize<32||layout.typeInfo<0||layout.type<0||layout.customName<0||layout.binaryData<0||
       layout.typeInfo+layout.customName+8>layout.wrapperSize||layout.typeInfo+layout.type+1>layout.wrapperSize||layout.binaryData+16>layout.wrapperSize)return false;
    std::int32_t messageId=0,readIndex=0;ArrayHeader wrappers;
    if(!memory.get(base+layout.messageId,messageId)||(messageFilter>=0&&messageId!=messageFilter)||
       !memory.get(base+layout.parameters+layout.wrappers,wrappers)||wrappers.count<0||wrappers.count>128||
       wrappers.capacity<wrappers.count||wrappers.capacity>512||
       (wrappers.count&&(!wrappers.data||wrappers.data<0x10000||(wrappers.data&7)))||
       !memory.get(base+layout.parameters+layout.readIndex,readIndex))return false;
    const auto hex=[](const std::uint8_t* data,std::size_t size){static const char digits[]="0123456789abcdef";std::string s;s.reserve(size*2);for(std::size_t i=0;i<size;++i){s+=digits[data[i]>>4];s+=digits[data[i]&15];}return s;};
    const auto parameterBytes=std::min<std::size_t>((std::size_t)layout.size,4096);std::vector<std::uint8_t> raw(parameterBytes);
    if(parameterBytes&&!memory.copy(base,raw.data(),raw.size()))return false;
    std::string rows="[";std::size_t capturedPayload=0;bool truncated=false;
    for(int i=0;i<wrappers.count;++i){
        const auto at=wrappers.data+std::uint64_t(i)*layout.wrapperSize;std::uint8_t kind=0;std::uint32_t name=0,nameNumber=0;ArrayHeader payload;
        if(!memory.get(at+layout.typeInfo+layout.type,kind)||!memory.get(at+layout.typeInfo+layout.customName,name)||
           !memory.get(at+layout.typeInfo+layout.customName+4,nameNumber)||!memory.get(at+layout.binaryData,payload)||
           payload.count<0||payload.count>16*1024*1024||payload.capacity<payload.count||payload.capacity>64*1024*1024||
           (payload.count&&(!payload.data||payload.data<0x10000)))return false;
        const auto take=std::min<std::size_t>((std::size_t)payload.count,capturedPayload<maxPayload?maxPayload-capturedPayload:0);
        std::vector<std::uint8_t> bytes(take);if(take&&!memory.copy(payload.data,bytes.data(),take))return false;
        capturedPayload+=take;const bool itemTruncated=take!=(std::size_t)payload.count;truncated=truncated||itemTruncated;
        if(i)rows+=',';const auto enumName=enumNames.find(kind);
        const auto customType=kind>=36&&kind<=38?syncResolveName(memory,namePool,nameLayout,{name,nameNumber}):std::string();
        rows+="{\"index\":"+std::to_string(i)+",\"type_value\":"+std::to_string(kind)+
            ",\"type_name\":"+(enumName==enumNames.end()?"null":quote(enumName->second))+
            ",\"custom_type_name_index\":"+std::to_string(name)+",\"custom_type_name_number\":"+std::to_string(nameNumber)+
            ",\"custom_type_name\":"+(customType.empty()?"null":quote(customType))+
            ",\"declared_payload_bytes\":"+std::to_string(payload.count)+",\"captured_payload_hex\":"+quote(hex(bytes.data(),bytes.size()))+
            ",\"truncated\":"+(itemTruncated?"true":"false")+"}";
    }
    rows+=']';output="{\"kind\":"+quote(rpcKind)+",\"capture_stage\":"+quote(stage)+",\"wire_packet_bytes_captured\":false,\"sample_id\":"+std::to_string(sampleId)+
        (functionPath?",\"function\":"+quote(functionPath):"")+(direction?",\"direction\":"+quote(direction):"")+
        (moduleSha256?",\"module_sha256\":"+quote(moduleSha256):"")+(functionPath?",\"message_type\":"+std::to_string(messageId):"")+
        ",\"tick_ms\":"+std::to_string(tickMs)+",\"message_id\":"+
        std::to_string(messageId)+",\"message_id_filter\":"+(messageFilter<0?"null":std::to_string(messageFilter))+
        ",\"object_address\":"+quote(ebon::hex(object))+",\"function_address\":"+quote(ebon::hex(function))+
        ",\"read_parameter_index\":"+std::to_string(readIndex)+",\"params_bytes_declared\":"+std::to_string(layout.size)+
        ",\"params_bytes_captured\":"+std::to_string(parameterBytes)+",\"params_truncated\":"+(parameterBytes!=(std::size_t)layout.size?"true":"false")+
        ",\"params_hex\":"+quote(hex(raw.data(),raw.size()))+
        ",\"wrappers\":"+rows+",\"payloads_truncated\":"+(truncated?"true":"false")+"}";
    return true;
}

class Capture {
    using ProcessEvent=void(__fastcall*)(void*,void*,void*);
    static inline std::atomic<Capture*> active_{nullptr};
    static inline std::atomic<ULONGLONG> armedUntil_{0};
    static inline std::atomic<std::uint64_t> globalSequence_{0};
    static inline thread_local std::uint64_t virtualSample_=0;
    const MemoryReader* memory_=nullptr;std::filesystem::path output_;
    std::uint64_t processEvent_=0,rootProcessEvent_=0,rpcAddress_=0,fogAddress_=0,friendClientAddress_=0,friendServerAddress_=0;unsigned vtableIndex_=0;
    ProcessEvent original_=nullptr;HMODULE pinnedModule_=nullptr;RpcLayout rpc_,friendClient_,friendServer_;FogLayout fog_;
    std::map<std::int64_t,std::string> enumNames_;std::set<std::uint64_t> hookedVtables_;
    std::mutex lock_;std::deque<std::string> queue_;std::size_t queuedBytes_=0,captured_=0,windowCaptured_=0;
    std::atomic<std::size_t> dropped_{0};
    std::size_t maxSamples_=32,maxPayload_=2*1024*1024;std::int32_t messageFilter_=-1;bool installed_=false,friendOnly_=false;
    std::uint64_t windowId_=0;std::string armSource_="not_armed";
    std::array<std::atomic<std::uint64_t>,2> friendCalls_{},friendSamples_{},friendRejects_{},friendFrameRejects_{};
    std::atomic<bool> expiryPublished_{false};
    std::string lastStatus_="not_initialized",bindingDiagnostics_="[]";
    unsigned windowSeconds_=30,nativeHooks_=0;
    NativeFrameLayout nativeFrame_;
    ProcessEvent rpcNative_=nullptr,fogNative_=nullptr,friendClientNative_=nullptr,friendServerNative_=nullptr;
    std::string nativeBindingStatus_="disabled",moduleSha256_;
    std::atomic<std::uint64_t> eventCalls_{0},windowEventCalls_{0},targetCalls_{0},nativeCalls_{0},windowNativeCalls_{0},parseRejected_{0},frameRejected_{0};
    std::map<std::uint64_t,std::string> functionNames_;
    std::map<std::uint64_t,std::size_t> windowFunctions_;
    struct SyncTarget {std::string name,path;RpcLayout rpc;bool replicated=false,targetActor=false;};
    std::map<std::uint64_t,SyncTarget> syncTargets_;
    ReplicatedPropertiesLayout replicatedLayout_;
    std::uint64_t namePool_=0;std::optional<Layout> nameLayout_;
    std::string syncBindings_="[]";
    std::map<std::string,std::size_t> capturedKinds_;
    std::atomic<std::uint64_t> windowSyncCalls_{0},syncSamples_{0};
    ULONGLONG lastStatePublish_=0;
    std::size_t lastReportedCaptured_=0;
    static const Type* type(const Reflection& r,const std::string& path){for(const auto& t:r.types)if(t.path==path)return &t;return nullptr;}
    static const Property* field(const Type* t,const char* name){if(t)for(const auto& p:t->fields)if(p.name==name)return &p;return nullptr;}
    static bool derives(const Type* t,const std::map<std::string,const Type*>& paths,const char* base){
        std::set<std::string> seen;for(;t&&seen.insert(t->path).second;){if(t->path==base)return true;auto i=paths.find(t->super);t=i==paths.end()?nullptr:i->second;}return false;
    }
    static bool processEventSlotMatches(const Reflection& reflection,unsigned index,const std::string& expected){
        for(const auto& row:reflection.runtimeVtables){
            const std::string marker="\"object\":\"/Script/CoreUObject.Default__Object\",\"entries\":[";
            auto pos=row.find(marker);if(pos==row.npos)continue;pos+=marker.size();
            for(unsigned slot=0;slot<=index;++slot){
                while(pos<row.size()&&std::isspace(static_cast<unsigned char>(row[pos])))++pos;
                const auto end=row.find_first_of(",]",pos);if(end==row.npos)return false;
                if(slot==index)return row.substr(pos,end-pos)==expected;pos=end+1;
            }
        }
        return false;
    }
    bool enqueue(std::string row,const std::string& kind="")noexcept{
        try{std::lock_guard<std::mutex> guard(lock_);constexpr std::size_t maxQueuedBytes=16*1024*1024;
            if(windowCaptured_>=maxSamples_||queue_.size()>=maxSamples_||row.size()>maxQueuedBytes||queuedBytes_>maxQueuedBytes-row.size()){++dropped_;return false;}
            queuedBytes_+=row.size();queue_.push_back(std::move(row));++captured_;++windowCaptured_;if(!kind.empty())++capturedKinds_[kind];return true;}
        catch(...){++dropped_;return false;}
    }
    bool onRpc(void* object,void* params,const char* stage="ProcessEvent_parameters"){if(!params)return false;std::string row;const auto id=globalSequence_.fetch_add(1,std::memory_order_relaxed)+1;
        if(captureClientGameDataRpc(*memory_,reinterpret_cast<std::uint64_t>(params),rpc_,messageFilter_,maxPayload_,enumNames_,
            reinterpret_cast<std::uint64_t>(object),rpcAddress_,id,GetTickCount64(),row,stage)){enqueue(std::move(row),"ClientGameDataRPC");return true;}
        ++parseRejected_;return false;}
    bool onFog(void* object,void* params,const char* stage="ProcessEvent_parameters"){
        if(!params)return false;std::string row;const auto sampleId=globalSequence_.fetch_add(1,std::memory_order_relaxed)+1;
        if(captureFogRecord(*memory_,reinterpret_cast<std::uint64_t>(params),fog_,reinterpret_cast<std::uint64_t>(object),fogAddress_,
                            sampleId,GetTickCount64(),row,stage)){enqueue(std::move(row),"OnRecChange_MiniMapFogRec");return true;}
        ++parseRejected_;return false;
    }
    bool onFriend(void* object,void* params,bool server,const char* stage="ProcessEvent_parameters"){
        const unsigned direction=server?1:0;++friendCalls_[direction];
        if(!params){++friendRejects_[direction];++parseRejected_;return false;}
        std::string row;const auto id=globalSequence_.fetch_add(1,std::memory_order_relaxed)+1;
        const auto name=server?"Server_Friend_RPC":"Client_Friend_RPC";
        const auto path=server?"/Script/HTGame.HTPlayerState.Server_Friend_RPC":"/Script/HTGame.HTPlayerState.Client_Friend_RPC";
        if(captureClientGameDataRpc(*memory_,reinterpret_cast<std::uint64_t>(params),server?friendServer_:friendClient_,-1,maxPayload_,enumNames_,
            reinterpret_cast<std::uint64_t>(object),server?friendServerAddress_:friendClientAddress_,id,GetTickCount64(),row,stage,
            name,path,server?"server":"client",moduleSha256_.c_str(),namePool_,nameLayout_?&*nameLayout_:nullptr)){
            if(enqueue(std::move(row),name))++friendSamples_[direction];return true;}
        ++friendRejects_[direction];++parseRejected_;return false;
    }
    bool onSync(void* object,void* params,std::uint64_t function,const SyncTarget& target){
        ++windowSyncCalls_;if(!params){++parseRejected_;return false;}
        std::string row;const auto id=globalSequence_.fetch_add(1,std::memory_order_relaxed)+1;
        const auto base=reinterpret_cast<std::uint64_t>(params),owner=reinterpret_cast<std::uint64_t>(object);
        const bool parsed=target.replicated?captureReplicatedProperties(*memory_,base,replicatedLayout_,owner,function,id,GetTickCount64(),
            maxPayload_,namePool_,nameLayout_?&*nameLayout_:nullptr,moduleSha256_.c_str(),row):
            captureClientGameDataRpc(*memory_,base,target.rpc,-1,maxPayload_,enumNames_,owner,function,id,GetTickCount64(),row,
                "ProcessEvent_parameters",target.name.c_str(),target.path.c_str(),"client",moduleSha256_.c_str());
        if(!parsed){++parseRejected_;return false;}
        if(!target.replicated){
            row.pop_back();row+=",\"map_coverage_verified\":false";
            if(target.targetActor){std::uint64_t actor=0;if(!memory_->get(base+32,actor)){++parseRejected_;return false;}
                row+=",\"target_actor_address\":"+quote(ebon::hex(actor));}
            row+='}';
        }
        if(enqueue(std::move(row),target.name)){++syncSamples_;return true;}return false;
    }
    static void __fastcall hook(void* object,void* function,void* params)noexcept{
        const auto previous=virtualSample_;auto* self=active_.load(std::memory_order_acquire);if(self){++self->eventCalls_;try{const auto address=reinterpret_cast<std::uint64_t>(function);
            if(GetTickCount64()<=armedUntil_.load(std::memory_order_relaxed)){
                ++self->windowEventCalls_;{std::lock_guard<std::mutex> guard(self->lock_);
                    if(self->windowFunctions_.size()<128||self->windowFunctions_.count(address))++self->windowFunctions_[address];}
                if(!self->friendOnly_&&address==self->rpcAddress_){++self->targetCalls_;if(self->onRpc(object,params))virtualSample_=address;}
                else if(!self->friendOnly_&&address==self->fogAddress_){++self->targetCalls_;if(self->onFog(object,params))virtualSample_=address;}
                else if(self->friendClientAddress_&&address==self->friendClientAddress_){++self->targetCalls_;if(self->onFriend(object,params,false))virtualSample_=address;}
                else if(self->friendServerAddress_&&address==self->friendServerAddress_){++self->targetCalls_;if(self->onFriend(object,params,true))virtualSample_=address;}
                else if(const auto sync=self->syncTargets_.find(address);sync!=self->syncTargets_.end()){
                    ++self->targetCalls_;if(self->onSync(object,params,address,sync->second))virtualSample_=address;}
            }
        }catch(...){}}
        if(self&&self->original_)self->original_(object,function,params);
        virtualSample_=previous;
    }
    std::uint64_t targetAddress(unsigned target)const{
        return target==1?fogAddress_:target==2?friendClientAddress_:target==3?friendServerAddress_:rpcAddress_;
    }
    bool sampleNativeFrame(void* object,void* frame,unsigned target){
        std::uint64_t parameters=0;
        if(!nativeFrameParameters(*memory_,reinterpret_cast<std::uint64_t>(frame),nativeFrame_,
            reinterpret_cast<std::uint64_t>(object),targetAddress(target),parameters)){
            ++frameRejected_;if(target>=2){++friendCalls_[target-2];++friendFrameRejects_[target-2];}return false;}
        if(target>=2)return onFriend(object,reinterpret_cast<void*>(parameters),target==3,"UFunction_native_parameters");
        return target==1?onFog(object,reinterpret_cast<void*>(parameters),"UFunction_native_parameters"):
                         onRpc(object,reinterpret_cast<void*>(parameters),"UFunction_native_parameters");
    }
    static void nativeCall(void* object,void* frame,void* result,unsigned target)noexcept{
        auto* self=active_.load(std::memory_order_acquire);if(!self)return;
        ++self->nativeCalls_;const auto function=self->targetAddress(target);
        const auto previous=virtualSample_;
        if(GetTickCount64()<=armedUntil_.load(std::memory_order_relaxed)){
            ++self->windowNativeCalls_;++self->targetCalls_;
            if(virtualSample_!=function)try{if(self->sampleNativeFrame(object,frame,target))virtualSample_=function;}
                catch(...){++self->frameRejected_;}
        }
        const auto original=target==1?self->fogNative_:target==2?self->friendClientNative_:target==3?self->friendServerNative_:self->rpcNative_;
        if(original)original(object,frame,result);
        virtualSample_=previous;
    }
    static void __fastcall rpcNativeHook(void* object,void* frame,void* result)noexcept{nativeCall(object,frame,result,false);}
    static void __fastcall fogNativeHook(void* object,void* frame,void* result)noexcept{nativeCall(object,frame,result,true);}
    static void __fastcall friendClientNativeHook(void* object,void* frame,void* result)noexcept{nativeCall(object,frame,result,2);}
    static void __fastcall friendServerNativeHook(void* object,void* frame,void* result)noexcept{nativeCall(object,frame,result,3);}
    void installNative(const MemoryReader& memory,const Reflection& reflection,const Config& config,const Module& module){
        if(!config.number("game_rpc_native_capture",0))return;
        const auto readOffset=[&](const char* key){const auto n=config.number(key,-1);if(n<0||n>512||(n&7))throw std::runtime_error("invalid native frame offset: "+std::string(key));return (std::int32_t)n;};
        nativeFrame_={readOffset("game_rpc_frame_node_offset"),readOffset("game_rpc_frame_object_offset"),readOffset("game_rpc_frame_code_offset"),
                      readOffset("game_rpc_frame_locals_offset"),readOffset("game_rpc_frame_current_function_offset")};
        const auto nativeOffset=config.number("function_native_offset",-1);
        if(nativeOffset<0||nativeOffset>512||(nativeOffset&7)){nativeBindingStatus_="native_offset_invalid";return;}
        const auto bind=[&](const Type* t,const char* key,ProcessEvent replacement,ProcessEvent& original){
            const auto rva=config.number(key,-1);std::uint64_t actual=0;
            if(!t||!t->nativeRva||rva<0||(std::uint64_t)rva>=module.size||*t->nativeRva!=(std::uint64_t)rva||
               !memory.executable(module.base+rva)||!memory.get(t->address+nativeOffset,actual)||actual!=module.base+rva)return false;
            auto* slot=reinterpret_cast<PVOID volatile*>(t->address+nativeOffset);DWORD protection=0;
            if(!VirtualProtect(const_cast<PVOID*>(slot),sizeof(void*),PAGE_READWRITE,&protection))return false;
            original=reinterpret_cast<ProcessEvent>(actual);
            const auto prior=InterlockedCompareExchangePointer(slot,reinterpret_cast<void*>(replacement),reinterpret_cast<void*>(actual));
            DWORD ignored=0;VirtualProtect(const_cast<PVOID*>(slot),sizeof(void*),protection,&ignored);
            if(prior!=reinterpret_cast<void*>(actual)){original=nullptr;return false;}++nativeHooks_;functionNames_[t->address]=t->path;return true;
        };
        const bool rpc=!friendOnly_&&bind(type(reflection,"/Script/HTGame.HTPlayerState.ClientGameDataRPC"),"game_rpc_native_rpc_rva",&rpcNativeHook,rpcNative_);
        const bool fog=!friendOnly_&&fog_.size&&bind(type(reflection,"/Script/HTGame.HTPlayerState.OnRecChange_MiniMapFogRec"),"game_rpc_native_fog_rva",&fogNativeHook,fogNative_);
        if(friendClientAddress_)bind(type(reflection,"/Script/HTGame.HTPlayerState.Client_Friend_RPC"),"game_rpc_native_friend_client_rva",&friendClientNativeHook,friendClientNative_);
        if(friendServerAddress_)bind(type(reflection,"/Script/HTGame.HTPlayerState.Server_Friend_RPC"),"game_rpc_native_friend_server_rva",&friendServerNativeHook,friendServerNative_);
        nativeBindingStatus_=rpc?(fog?"rpc_and_fog_installed":"rpc_installed_fog_unavailable"):"rpc_native_binding_unverified";
        if(friendOnly_)nativeBindingStatus_=friendClientNative_&&friendServerNative_?"friend_client_and_server_installed":"friend_native_binding_incomplete";
    }
    void state(const char* status,bool notify=true){std::size_t queued=0,captured=0,dropped=0,windowCaptured=0;std::string functions="[",kinds="{";
        std::array<std::size_t,2> friendTotals{};
        {std::lock_guard<std::mutex> guard(lock_);queued=queue_.size();captured=captured_;dropped=dropped_;windowCaptured=windowCaptured_;
            for(unsigned i=0;i<2;++i){const auto found=capturedKinds_.find(i?"Server_Friend_RPC":"Client_Friend_RPC");if(found!=capturedKinds_.end())friendTotals[i]=found->second;}
            bool firstKind=true;for(const auto& item:capturedKinds_){if(!firstKind)kinds+=',';firstKind=false;kinds+=quote(item.first)+":"+std::to_string(item.second);}
            bool first=true;for(const auto& item:windowFunctions_){if(!first)functions+=',';first=false;const auto name=functionNames_.find(item.first);
                functions+="{\"function\":"+(name==functionNames_.end()?quote(ebon::hex(item.first)):quote(name->second))+",\"calls\":"+std::to_string(item.second)+"}";}}
        functions+=']';kinds+='}';
        const auto clientSamples=friendSamples_[0].load(),serverSamples=friendSamples_[1].load();
        const auto friendCalls=friendCalls_[0].load()+friendCalls_[1].load();
        const char* friendDiagnosis=clientSamples?"client_friend_sample_saved":serverSamples?"request_only_no_client_response":
            friendOnly_&&frameRejected_.load()?"friend_native_frames_rejected":!friendCalls?"friend_functions_not_observed":
            friendRejects_[0].load()+friendRejects_[1].load()?"friend_parameters_rejected":"friend_samples_dropped";
        std::string friends="[";
        for(unsigned i=0;i<2;++i){if(i)friends+=',';friends+="{\"function\":"+quote(i?"Server_Friend_RPC":"Client_Friend_RPC")+
            ",\"window_calls\":"+std::to_string(friendCalls_[i].load())+",\"window_samples\":"+std::to_string(friendSamples_[i].load())+
            ",\"window_parse_rejected\":"+std::to_string(friendRejects_[i].load())+
            ",\"window_frame_rejected\":"+std::to_string(friendFrameRejects_[i].load())+",\"samples_total\":"+std::to_string(friendTotals[i])+"}";}
        friends+=']';
        save(output_/"game_rpc_sample_state.json","{\"format\":\"EbonGameRpcSampleCapture-v1\",\"status\":"+quote(status)+",\"hotkey\":\"F8\",\"window_seconds\":"+std::to_string(windowSeconds_)+",\"rpc_target_verified\":"+(rpcAddress_?"true":"false")+
            ",\"installed\":"+(installed_?"true":"false")+
            ",\"sampler_revision\":\"friend-capture-v2\",\"capture_scope\":"+quote(friendOnly_?"friends":"gameplay")+
            ",\"process_id\":"+std::to_string(GetCurrentProcessId())+",\"heartbeat_tick_ms\":"+std::to_string(GetTickCount64())+
            ",\"window_id\":"+std::to_string(windowId_)+",\"arm_source\":"+quote(armSource_)+
            ",\"armed_until_tick_ms\":"+std::to_string(armedUntil_.load())+
            ",\"friend_diagnosis\":"+quote(friendDiagnosis)+",\"friend_counters\":"+friends+
            ",\"module_sha256\":"+quote(moduleSha256_)+
            ",\"friend_client_schema_verified\":"+(friendClientAddress_?"true":"false")+",\"friend_server_schema_verified\":"+(friendServerAddress_?"true":"false")+
            ",\"friend_client_native_installed\":"+(friendClientNative_?"true":"false")+",\"friend_server_native_installed\":"+(friendServerNative_?"true":"false")+
            ",\"fog_callback_target_observed\":"+(fogAddress_?"true":"false")+",\"fog_parameter_schema_verified\":"+(fog_.size?"true":"false")+",\"sample_limit_per_window\":"+std::to_string(maxSamples_)+",\"payload_limit_bytes_per_rpc\":"+std::to_string(maxPayload_)+
            ",\"process_event_vtable_index\":"+std::to_string(vtableIndex_)+
            ",\"hooked_vtables\":"+std::to_string(hookedVtables_.size())+",\"message_id_filter\":"+(messageFilter_<0?"null":std::to_string(messageFilter_))+
            ",\"root_process_event\":"+quote(ebon::hex(rootProcessEvent_))+",\"playerstate_process_event\":"+quote(ebon::hex(processEvent_))+
            ",\"binding_diagnostics\":"+bindingDiagnostics_+
            ",\"sync_targets\":"+syncBindings_+",\"window_sync_calls\":"+std::to_string(windowSyncCalls_.load())+
            ",\"sync_samples_total\":"+std::to_string(syncSamples_.load())+",\"captured_by_kind\":"+kinds+",\"map_coverage_verified\":false"+
            ",\"native_hooks\":"+std::to_string(nativeHooks_)+",\"native_binding_status\":"+quote(nativeBindingStatus_)+
            ",\"event_calls_total\":"+std::to_string(eventCalls_.load())+",\"window_event_calls\":"+std::to_string(windowEventCalls_.load())+
            ",\"native_calls_total\":"+std::to_string(nativeCalls_.load())+",\"window_native_calls\":"+std::to_string(windowNativeCalls_.load())+
            ",\"window_target_calls\":"+std::to_string(targetCalls_.load())+",\"window_parse_rejected\":"+std::to_string(parseRejected_.load())+
            ",\"window_native_frame_rejected\":"+std::to_string(frameRejected_.load())+
            ",\"native_frame_layout\":{\"node_offset\":"+std::to_string(nativeFrame_.node)+",\"object_offset\":"+std::to_string(nativeFrame_.object)+",\"locals_pointer_offset\":"+std::to_string(nativeFrame_.locals)+"},\"window_functions\":"+functions+
            ",\"queued_samples\":"+std::to_string(queued)+",\"window_samples\":"+std::to_string(windowCaptured)+",\"captured_total\":"+std::to_string(captured)+",\"dropped_total\":"+std::to_string(dropped)+",\"wire_packet_bytes_captured\":false}\n");
        lastStatePublish_=GetTickCount64();const bool changed=lastStatus_!=status;lastStatus_=status;if(!notify)return;
        if(!changed&&(lastStatus_!="capturing"||lastReportedCaptured_==captured))return;lastReportedCaptured_=captured;
        const char* explanation="采样器安装失败，F8 不可用；请查看状态文件中的失败原因。";
        if(lastStatus_=="ready_press_f8")explanation="采样器已就绪，请在游戏或日志窗口按 F8 开启采样。";
        else if(lastStatus_=="armed")explanation="采样窗口已开启，请执行会触发正常同步的游戏操作。";
        else if(lastStatus_=="capturing")explanation="已收到并写入样本，继续监听本次窗口。";
        else if(lastStatus_=="window_expired")explanation=windowCaptured?"采样窗口已结束，可重新按 F8 开始。":
            targetCalls_.load()?"目标入口有调用，但没有保存样本；请查看拒绝及丢弃计数。":
            windowEventCalls_.load()?"监听入口有调用，但未见目标函数；可重新按 F8。":"两个监听入口均无窗口内调用；请核对同步时间和调用路径。";
        else if(lastStatus_=="no_verified_playerstate_vtable")explanation="PlayerState 事件入口与版本配置不匹配，F8 不可用；实际地址已写入状态文件。";
        else if(lastStatus_=="playerstate_vtable_patch_failed")explanation="已验证 PlayerState 事件入口，但未能安装采样器；系统错误已写入状态文件。";
        console::logf(installed_&&!(lastStatus_=="window_expired"&&!windowCaptured)?"success":"warn",
                      "[F8] status=%s | %s 窗口=%u秒，本次=%zu，总计=%zu，丢弃=%zu；PE调用=%llu，native调用=%llu，参数拒绝=%llu，帧拒绝=%llu，同步样本=%llu（不代表地图完整）",
                      status,explanation,windowSeconds_,windowCaptured,captured,dropped,
                      (unsigned long long)windowEventCalls_.load(),(unsigned long long)windowNativeCalls_.load(),
                      (unsigned long long)parseRejected_.load(),(unsigned long long)frameRejected_.load(),(unsigned long long)syncSamples_.load());
        if(friendOnly_)console::logf(clientSamples?"success":"warn","[Friend] revision=friend-capture-v2 window=%llu client=%llu server=%llu diagnosis=%s; samples are arguments, not wire packets",
            (unsigned long long)windowId_,(unsigned long long)clientSamples,(unsigned long long)serverSamples,friendDiagnosis);
    }
    bool installImpl(const MemoryReader& memory,const Reflection& reflection,const Config& config,const Module& module,const std::filesystem::path& output,std::string& status){
        bindingDiagnostics_="[]";
        if(!config.number("game_rpc_sample_capture",0)){status="disabled";return false;}
        if(installed_){status=lastStatus_;return true;}
        if(config.text("exe_sha256")!=reflection.moduleSha256){status="profile_hash_mismatch";return false;}
        moduleSha256_=reflection.moduleSha256;
        friendOnly_=config.number("game_rpc_sample_friend_only",0)!=0;
        const auto index=config.number("process_event_vtable_index",-1);const auto rvaText=config.text("process_event_rva");
        if(index<0||index>255||rvaText.empty()){status="process_event_binding_missing";return false;}
        std::uint64_t rva=0,playerRva=0;try{
            rva=std::stoull(rvaText,nullptr,0);
            playerRva=std::stoull(config.text("process_event_playerstate_rva",rvaText),nullptr,0);
        }catch(...){status="process_event_rva_invalid";return false;}
        if(!module.base||rva>=module.size||playerRva>=module.size||!memory.executable(module.base+rva)||
           !memory.executable(module.base+playerRva)){status="process_event_target_unavailable";return false;}
        rootProcessEvent_=module.base+rva;processEvent_=module.base+playerRva;vtableIndex_=(unsigned)index;
        if(!processEventSlotMatches(reflection,(unsigned)index,quote(ebon::hex(rva)))){status="process_event_root_vtable_mismatch";return false;}
        const auto rpc=type(reflection,"/Script/HTGame.HTPlayerState.ClientGameDataRPC"),params=type(reflection,"/Script/HottaFramework.ParameterWrapperArray");
        const auto wrapper=type(reflection,"/Script/HottaFramework.ParameterWrapper"),info=type(reflection,"/Script/HottaFramework.ParameterTypeInfo");
        const auto id=field(rpc,"InMsgID"),in=field(rpc,"InParameters"),array=field(params,"ParameterWrappers"),readIndex=field(params,"ReadParameterIndex");
        const auto wrapperInfo=field(wrapper,"TypeInfo"),binary=field(wrapper,"BinaryData"),kind=field(info,"Type"),customName=field(info,"CustomTypeName");
        if(!rpc||!rpc->size||!rpc->parameterSize||!params||!params->size||!wrapper||!wrapper->size||!info||!info->size||!id||!in||!array||!readIndex||!wrapperInfo||!binary||!kind||!customName||
           id->type!="IntProperty"||id->offset<0||id->offset+4>(int)*rpc->parameterSize||
           in->target!=params->path||in->offset<0||in->offset+(int)*params->size>(int)*rpc->parameterSize||
           readIndex->offset<0||readIndex->offset+4>(int)*params->size||array->offset<0||array->offset+16>(int)*params->size||
           !wrapperArrayLayoutMatches(array,*wrapper->size)||
           wrapperInfo->target!=info->path||wrapperInfo->offset<0||wrapperInfo->offset+12>(int)*wrapper->size||
           binary->type!="ArrayProperty"||binary->offset<0||binary->offset+16>(int)*wrapper->size||
           binary->detail.find("\"type\":\"ByteProperty\"")==std::string::npos||
           kind->type!="EnumProperty"||kind->offset!=0||customName->type!="NameProperty"||customName->offset!=4||customName->offset+8>(int)*info->size){status="parameter_schema_unverified";return false;}
        rpc_={*rpc->parameterSize,id->offset,in->offset,array->offset,readIndex->offset,*wrapper->size,wrapperInfo->offset,kind->offset,customName->offset,binary->offset};
        const auto message=type(reflection,"/Script/HottaFramework.HottaRpcMessage");
        const auto clientFriend=type(reflection,"/Script/HTGame.HTPlayerState.Client_Friend_RPC"),serverFriend=type(reflection,"/Script/HTGame.HTPlayerState.Server_Friend_RPC");
        if(friendRpcLayout(clientFriend,message,params,wrapper,info,friendClient_))friendClientAddress_=clientFriend->address;
        if(friendRpcLayout(serverFriend,message,params,wrapper,info,friendServer_))friendServerAddress_=serverFriend->address;
        if(friendOnly_&&(!friendClientAddress_||!friendServerAddress_)){status="friend_parameter_schema_unverified";return false;}
        namePool_=reflection.poolAddress();nameLayout_=reflection.layout();
        if(!friendOnly_&&config.number("game_rpc_sync_capture",0)){
            syncBindings_="[";bool first=true;
            for(const auto name:{"ClientDataLayerRPC","ClientCommonFunRPC","SendReplicatedObjectPropertyArrayToClient"}){
                SyncTarget target;target.name=name;target.path="/Script/HTGame.HTPlayerState."+target.name;
                target.replicated=target.name=="SendReplicatedObjectPropertyArrayToClient";target.targetActor=target.name=="ClientCommonFunRPC";
                const auto function=type(reflection,target.path);bool verified=false;
                if(target.replicated){replicatedLayout_=replicatedPropertiesLayout(function,type(reflection,"/Script/HTGame.HTReplicatedObjectPropertyContainer"),
                    type(reflection,"/Script/HottaFramework.HottaReplicatedObjectPropertyContainer"),type(reflection,"/Script/HottaFramework.HottaReplicatedObjectProperty"));verified=replicatedLayout_.verified;}
                else verified=friendRpcLayout(function,message,params,wrapper,info,target.rpc,target.targetActor);
                if(!first)syncBindings_+=',';first=false;syncBindings_+="{\"function\":"+quote(target.path)+",\"schema_verified\":"+(verified?"true":"false")+"}";
                if(verified)syncTargets_.emplace(function->address,std::move(target));
            }
            syncBindings_+=']';
        }
        if(const auto t=type(reflection,"/Script/HTGame.HTPlayerState.OnRecChange_MiniMapFogRec")){
            const auto n=field(t,"RecordName"),c=field(t,"ChangeType"),r=field(t,"RowIndex"),old=field(t,"OldRow");
            if(t->parameterSize&&n&&n->type=="StrProperty"&&n->offset>=0&&n->offset+16<=*t->parameterSize&&
               c&&c->type=="IntProperty"&&c->offset>=0&&c->offset+4<=*t->parameterSize&&
               r&&r->type=="IntProperty"&&r->offset>=0&&r->offset+4<=*t->parameterSize&&
               old&&old->type=="StructProperty"&&old->offset>=0&&old->element>0&&old->offset+old->element<=*t->parameterSize)
                fog_={*t->parameterSize,n->offset,c->offset,r->offset,old->offset,old->element};
        }
        const auto filter=config.number("game_rpc_sample_candidate_msg_id",-1),limit=config.number("game_rpc_sample_limit",32),payloadMb=config.number("game_rpc_sample_max_payload_mb",2);
        if(filter< -1||filter>1000000000||limit<1||limit>256||payloadMb<1||payloadMb>8){status="sample_limits_invalid";return false;}
        const auto seconds=config.number("game_rpc_sample_window_seconds",30);
        if(seconds<1||seconds>600){status="sample_window_invalid";return false;}windowSeconds_=(unsigned)seconds;
        messageFilter_=(std::int32_t)filter;maxSamples_=(std::size_t)limit;maxPayload_=(std::size_t)payloadMb*1024*1024;
        if(const auto e=type(reflection,"/Script/HottaFramework.EParameterType"))for(const auto& item:e->enumValues)enumNames_[item.second]=item.first;
        memory_=&memory;output_=output;rpcAddress_=rpc->address;
        if(const auto f=type(reflection,"/Script/HTGame.HTPlayerState.OnRecChange_MiniMapFogRec"))fogAddress_=f->address;
        std::filesystem::create_directories(output_);
        {std::ofstream out(output_/"game_rpc_samples.jsonl",std::ios::binary|std::ios::app);if(!out){status="sample_file_unavailable";return false;}}
        HMODULE self=nullptr;if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,reinterpret_cast<LPCWSTR>(&hook),&self)){status="cannot_pin_sampler_module";return false;}
        pinnedModule_=self;
        std::map<std::uint64_t,const Type*> byAddress;std::map<std::string,const Type*> byPath;
        for(const auto& t:reflection.types){byAddress[t.address]=&t;if(t.kind=="class")byPath[t.path]=&t;
            if(t.kind=="function")functionNames_[t.address]=t.path;}
        unsigned candidates=0;std::set<std::uint64_t> vtables;std::string diagnostics;
        for(const auto& o:reflection.objects){if(o.name.rfind("Default__",0)!=0)continue;auto t=byAddress.find(o.klass);
            if(t==byAddress.end()||!derives(t->second,byPath,"/Script/HTGame.HTPlayerState"))continue;++candidates;
            std::uint64_t vtable=0,target=0;const bool readable=memory.get(o.address,vtable)&&vtable>=0x10000&&!(vtable&7)&&
                memory.get(vtable+vtableIndex_*8,target);
            const bool verified=readable&&(target==processEvent_||
                (hookedVtables_.count(vtable)&&target==reinterpret_cast<std::uint64_t>(&hook)));
            if(!diagnostics.empty())diagnostics+=',';
            diagnostics+="{\"object\":"+quote(o.path)+",\"vtable\":"+quote(ebon::hex(vtable))+",\"observed_target\":"+quote(ebon::hex(target))+
                ",\"expected_target\":"+quote(ebon::hex(processEvent_))+",\"readable\":"+(readable?"true":"false")+",\"verified\":"+(verified?"true":"false");
            if(!verified){diagnostics+="}";continue;}
            if(!vtables.insert(vtable).second){diagnostics+="}";continue;}
            if(!original_)original_=reinterpret_cast<ProcessEvent>(target);auto* slot=reinterpret_cast<void**>(vtable+vtableIndex_*8);DWORD protection=0;
            if(!VirtualProtect(slot,sizeof(void*),PAGE_READWRITE,&protection)){diagnostics+=",\"patch_error\":"+std::to_string(GetLastError())+"}";continue;}
            active_.store(this,std::memory_order_release);
            const auto prior=reinterpret_cast<void*>(InterlockedCompareExchangePointer(reinterpret_cast<PVOID volatile*>(slot),reinterpret_cast<void*>(&hook),reinterpret_cast<void*>(processEvent_)));
            DWORD ignored=0;VirtualProtect(slot,sizeof(void*),protection,&ignored);if(prior==reinterpret_cast<void*>(processEvent_))hookedVtables_.insert(vtable);
            diagnostics+=",\"patched\":"+std::string(prior==reinterpret_cast<void*>(processEvent_)?"true":"false")+"}";
        }
        bindingDiagnostics_="["+diagnostics+"]";
        if(hookedVtables_.empty()){active_.store(nullptr,std::memory_order_release);if(pinnedModule_){FreeLibrary(pinnedModule_);pinnedModule_=nullptr;}
            status=!vtables.empty()?"playerstate_vtable_patch_failed":candidates?"no_verified_playerstate_vtable":"playerstate_cdo_not_observed";return false;}
        try{installNative(memory,reflection,config,module);}catch(const std::exception& e){nativeBindingStatus_="native_install_error";console::logf("warn","F8 native binding failed: %s",e.what());}
        installed_=true;status="ready_press_f8";return true;
    }
public:
    bool install(const MemoryReader& memory,const Reflection& reflection,const Config& config,const Module& module,const std::filesystem::path& output,std::string& status){
        if(installed_){status=lastStatus_;return true;}
        output_=output;lastStatus_="error";std::filesystem::create_directories(output_);
        const bool result=installImpl(memory,reflection,config,module,output,status);
        state(status.c_str());
        if(result&&config.number("game_rpc_sample_auto_arm",0)){console::log("info","F8 自动采样已启用，开始监听登录及首次同步；F8 可重新开启窗口。");arm("automatic");}
        return result;
    }
    void reportStatus()const{
        console::logf(installed_?"success":"warn","[F8] status=%s | %s 状态文件：%s",lastStatus_.c_str(),
                      installed_?"采样器已安装；窗口状态如上，按 F8 可重新开启采样。":"采样器未安装，F8 不可用；继续等待不会自动就绪。",
                      (output_/"game_rpc_sample_state.json").u8string().c_str());
    }
    bool installed()const{return installed_;}
    void arm(const char* source="hotkey"){armedUntil_.store(0,std::memory_order_release);flush();
        {std::lock_guard<std::mutex> guard(lock_);windowCaptured_=0;windowFunctions_.clear();}
        windowEventCalls_=0;windowNativeCalls_=0;targetCalls_=0;parseRejected_=0;frameRejected_=0;windowSyncCalls_=0;
        for(unsigned i=0;i<2;++i){friendCalls_[i]=0;friendSamples_[i]=0;friendRejects_[i]=0;friendFrameRejects_[i]=0;}
        ++windowId_;armSource_=source;
        armedUntil_.store(GetTickCount64()+(ULONGLONG)windowSeconds_*1000,std::memory_order_release);expiryPublished_.store(false,std::memory_order_release);state("armed");}
    void pollRequest(){
        const auto request=output_/"rpc_sample_capture.request";std::error_code error;
        const bool exists=std::filesystem::exists(request,error);
        if(error)throw std::runtime_error("cannot inspect RPC sample request: "+error.message());
        if(!exists)return;
        if(!std::filesystem::remove(request,error)||error)throw std::runtime_error("cannot consume RPC sample request");
        arm("request_file");
    }
    void flush(){std::deque<std::string> pending;{std::lock_guard<std::mutex> guard(lock_);pending.swap(queue_);queuedBytes_=0;}
        if(!pending.empty()){try{std::ofstream out(output_/"game_rpc_samples.jsonl",std::ios::binary|std::ios::app);if(!out)throw std::runtime_error("cannot append Game RPC sample output");
                for(const auto& row:pending)out<<row<<'\n';out.flush();if(!out)throw std::runtime_error("cannot flush Game RPC samples");}
            catch(...){std::lock_guard<std::mutex> guard(lock_);while(!pending.empty()){queuedBytes_+=pending.back().size();queue_.push_front(std::move(pending.back()));pending.pop_back();}throw;}
            state("capturing");}
        const auto until=armedUntil_.load(std::memory_order_acquire);
        if(until&&GetTickCount64()>until&&!expiryPublished_.exchange(true,std::memory_order_acq_rel))state("window_expired");
        // Expired windows still publish liveness so a request file can reopen them later.
        else if(until&&GetTickCount64()-lastStatePublish_>=1000){const auto status=lastStatus_;state(status.c_str(),false);}
    }
};
} // namespace ebon::rpc_samples
