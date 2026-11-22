// Exercises resource values and native sparse containers without a game process.
#include "../dumper/ebon_resource_dump.hpp"
#include "../dumper/ebon_rpc_sample_capture.hpp"
#include <cassert>

namespace {
void require(bool value,const char* message) {
    if(!value)throw std::runtime_error(message);
}

std::string contents(const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary);
    require(bool(file),"self-test output is missing");
    return std::string((std::istreambuf_iterator<char>(file)),{});
}

volatile LONG samplerRootCalls=0,samplerOverrideCalls=0,samplerNativeCalls=0;
std::uint64_t samplerRpcFunction=0,samplerFogFunction=0;
void __fastcall samplerNative(void*,void*,void* result) {
    InterlockedIncrement(&samplerNativeCalls);if(result)*static_cast<int*>(result)=77;
}
void invokeSamplerNative(void* object,std::uint64_t function,void* params,void* result=nullptr,std::uint64_t code=0) {
    using Event=void(__fastcall*)(void*,void*,void*);
    alignas(8) std::array<std::uint64_t,20> frame{};
    frame[2]=function;frame[3]=reinterpret_cast<std::uint64_t>(object);frame[4]=code;
    frame[5]=reinterpret_cast<std::uint64_t>(params);frame[18]=function;
    const auto slot=reinterpret_cast<std::uint64_t*>(function+216);
    reinterpret_cast<Event>(*slot)(object,frame.data(),result);
}
void* samplerLastObject=nullptr;void* samplerLastFunction=nullptr;void* samplerLastParams=nullptr;
void __fastcall samplerRoot(void* object,void* function,void* params) {
    InterlockedIncrement(&samplerRootCalls);
    samplerLastObject=object;samplerLastFunction=function;samplerLastParams=params;
}
void __fastcall samplerOverride(void* object,void* function,void* params) {
    InterlockedIncrement(&samplerOverrideCalls);samplerRoot(object,function,params);
    const auto address=reinterpret_cast<std::uint64_t>(function);
    if(address==samplerRpcFunction||address==samplerFogFunction)invokeSamplerNative(object,address,params);
}

void testRpcSamplerBinding(const std::filesystem::path& output) {
    static ebon::InProcessReader memory;
    const auto module=ebon::findModule(GetCurrentProcessId(),L"");
    const auto root=reinterpret_cast<std::uint64_t>(&samplerRoot),derived=reinterpret_cast<std::uint64_t>(&samplerOverride);
    ebon::Config config;config.values={{"game_rpc_sample_capture","1"},{"exe_sha256","fixture"},
        {"process_event_vtable_index","76"},{"process_event_rva",ebon::hex(root-module.base)}};
    ebon::Reflection reflection(memory,config,module);reflection.moduleSha256="fixture";
    std::string rootTable="{\"object\":\"/Script/CoreUObject.Default__Object\",\"entries\":[";
    for(unsigned i=0;i<=76;++i){if(i)rootTable+=',';rootTable+=i==76?ebon::quote(ebon::hex(root-module.base)):"null";}
    reflection.runtimeVtables.push_back(rootTable+"]}");
    constexpr std::uint64_t ownerAddress=0x50000;
    alignas(8) static std::array<std::uint64_t,32> rpcFunction{},fogFunction{};
    const auto rpcAddress=reinterpret_cast<std::uint64_t>(rpcFunction.data()),fogAddress=reinterpret_cast<std::uint64_t>(fogFunction.data());
    samplerRpcFunction=rpcAddress;samplerFogFunction=fogAddress;
    rpcFunction[27]=fogFunction[27]=reinterpret_cast<std::uint64_t>(&samplerNative);
    const auto addType=[&](const char* kind,const char* path,int size,std::uint64_t address){
        ebon::Type t;t.kind=kind;t.path=path;t.size=size;t.address=address;return t;
    };
    const auto field=[](const char* name,const char* type,int offset,int size,const char* target=""){
        ebon::Property p;p.name=name;p.type=type;p.offset=offset;p.element=size;p.array=1;p.target=target;return p;
    };
    auto owner=addType("class","/Script/HTGame.HTPlayerState",64,ownerAddress);
    auto rpc=addType("function","/Script/HTGame.HTPlayerState.ClientGameDataRPC",32,rpcAddress);rpc.parameterSize=std::uint16_t(32);
    rpc.nativeRva=reinterpret_cast<std::uint64_t>(&samplerNative)-module.base;
    rpc.fields={field("InMsgID","IntProperty",0,4),field("InParameters","StructProperty",8,24,"/Script/HottaFramework.ParameterWrapperArray")};
    auto params=addType("struct","/Script/HottaFramework.ParameterWrapperArray",24,0x61000);
    auto array=field("ParameterWrappers","ArrayProperty",0,16);
    array.detail="{\"inner\":{\"type\":\"StructProperty\",\"size\":32,\"target\":\"/Script/HottaFramework.ParameterWrapper\"}}";
    params.fields={array,field("ReadParameterIndex","IntProperty",16,4)};
    auto wrapper=addType("struct","/Script/HottaFramework.ParameterWrapper",32,0x62000);
    auto binary=field("BinaryData","ArrayProperty",16,16);binary.detail="{\"inner\":{\"type\":\"ByteProperty\"}}";
    wrapper.fields={field("TypeInfo","StructProperty",0,12,"/Script/HottaFramework.ParameterTypeInfo"),binary};
    auto info=addType("struct","/Script/HottaFramework.ParameterTypeInfo",12,0x63000);
    info.fields={field("Type","EnumProperty",0,1),field("CustomTypeName","NameProperty",4,8)};
    auto fog=addType("function","/Script/HTGame.HTPlayerState.OnRecChange_MiniMapFogRec",64,fogAddress);fog.parameterSize=std::uint16_t(64);fog.nativeRva=rpc.nativeRva;
    fog.fields={field("RecordName","StrProperty",0,16),field("ChangeType","IntProperty",16,4),field("RowIndex","IntProperty",20,4),field("OldRow","StructProperty",24,40)};
    reflection.types={owner,rpc,params,wrapper,info,fog};
    auto message=addType("struct","/Script/HottaFramework.HottaRpcMessage",32,0x64000);message.super=params.path;
    message.fields={field("Type","IntProperty",24,4)};
    auto dataLayer=addType("function","/Script/HTGame.HTPlayerState.ClientDataLayerRPC",32,0x65000);dataLayer.parameterSize=std::uint16_t(32);
    dataLayer.fields={field("InMessage","StructProperty",0,32,message.path.c_str())};
    auto common=addType("function","/Script/HTGame.HTPlayerState.ClientCommonFunRPC",40,0x66000);common.parameterSize=std::uint16_t(40);
    common.fields={field("InMessage","StructProperty",0,32,message.path.c_str()),field("InTargetActor","ObjectProperty",32,8,"/Script/Engine.Actor")};
    auto propertiesRpc=addType("function","/Script/HTGame.HTPlayerState.SendReplicatedObjectPropertyArrayToClient",16,0x67000);propertiesRpc.parameterSize=std::uint16_t(16);
    auto containers=field("InObjectPropertyContainerArray","ArrayProperty",0,16);
    containers.detail="{\"inner\":{\"type\":\"StructProperty\",\"size\":96,\"target\":\"/Script/HTGame.HTReplicatedObjectPropertyContainer\"}}";
    propertiesRpc.fields={containers};
    auto propertyBase=addType("struct","/Script/HottaFramework.HottaReplicatedObjectPropertyContainer",32,0x68000);
    auto properties=field("Properties","ArrayProperty",8,16);properties.detail="{\"inner\":{\"type\":\"StructProperty\",\"size\":32,\"target\":\"/Script/HottaFramework.HottaReplicatedObjectProperty\"}}";
    propertyBase.fields={properties,field("Owner","ObjectProperty",24,8,"/Script/CoreUObject.Object")};
    auto propertyContainer=addType("struct","/Script/HTGame.HTReplicatedObjectPropertyContainer",96,0x69000);propertyContainer.super=propertyBase.path;
    propertyContainer.fields={field("GameplayModuleId","NameProperty",84,8)};
    auto propertyValue=addType("struct","/Script/HottaFramework.HottaReplicatedObjectProperty",32,0x6a000);
    auto byteData=field("Datas","ArrayProperty",8,16);byteData.detail="{\"inner\":{\"type\":\"ByteProperty\",\"size\":1}}";
    auto compressed=field("bCompressed","BoolProperty",24,1);compressed.boolMask=255;
    propertyValue.fields={field("Name","NameProperty",0,8),byteData,compressed};
    for(const auto& t:{message,dataLayer,common,propertiesRpc,propertyBase,propertyContainer,propertyValue})reflection.types.push_back(t);
    config.values["game_rpc_sync_capture"]="1";
    const auto propertiesLayout=ebon::rpc_samples::replicatedPropertiesLayout(&propertiesRpc,&propertyContainer,&propertyBase,&propertyValue);
    require(propertiesLayout.verified,"observed replicated-property schema was not recognized");
    auto invalidProperty=propertyValue;invalidProperty.fields[1].offset=16;
    require(!ebon::rpc_samples::replicatedPropertiesLayout(&propertiesRpc,&propertyContainer,&propertyBase,&invalidProperty).verified,
            "incorrect replicated-property array offset was accepted");
    ebon::rpc_samples::RpcLayout commonLayout;
    require(ebon::rpc_samples::friendRpcLayout(&common,&message,&params,&wrapper,&info,commonLayout,true)&&commonLayout.size==40,
            "observed common-function RPC schema was rejected");
    auto invalidCommon=common;invalidCommon.fields[1].offset=24;
    require(!ebon::rpc_samples::friendRpcLayout(&invalidCommon,&message,&params,&wrapper,&info,commonLayout,true),
            "incorrect common-function actor offset was accepted");
    alignas(8) static std::array<std::uint64_t,96> table{};table[76]=derived;
    alignas(8) static std::uint64_t object=reinterpret_cast<std::uint64_t>(table.data());
    ebon::Object cdo;cdo.address=reinterpret_cast<std::uint64_t>(&object);cdo.klass=ownerAddress;
    cdo.name="Default__HTPlayerState";cdo.path="/Script/HTGame.Default__HTPlayerState";
    reflection.objects={cdo};
    ebon::console::setLogFile(output/"rpc-binding.log");
    std::string status;
    {
        ebon::rpc_samples::Capture rejected;
        require(!rejected.install(memory,reflection,config,module,output/"rpc-binding-rejected",status)&&
                status=="no_verified_playerstate_vtable"&&table[76]==derived,
                "unlisted PlayerState override was accepted or modified");
        const auto state=contents(output/"rpc-binding-rejected"/"game_rpc_sample_state.json");
        require(state.find("\"installed\":false")!=state.npos&&state.find("\"observed_target\":"+ebon::quote(ebon::hex(derived)))!=state.npos,
                "failed binding lost its actual target diagnostics");
    }
    config.values["process_event_playerstate_rva"]=ebon::hex(derived-module.base);
    {
        auto bad=config;bad.values["process_event_rva"]=ebon::hex(derived-module.base);
        ebon::rpc_samples::Capture rejected;
        require(!rejected.install(memory,reflection,bad,module,output/"rpc-root-rejected",status)&&
                status=="process_event_root_vtable_mismatch"&&table[76]==derived,
                "PlayerState override bypassed root UObject binding verification");
    }
    auto shared=cdo;shared.name="Default__SharedPlayerState";shared.path="/Script/Fixture.Default__SharedPlayerState";
    reflection.objects.push_back(shared);
    config.values["game_rpc_native_capture"]="1";config.values["function_native_offset"]="216";
    config.values["game_rpc_native_rpc_rva"]=config.values["game_rpc_native_fog_rva"]=ebon::hex(*rpc.nativeRva);
    config.values["game_rpc_frame_node_offset"]="16";config.values["game_rpc_frame_object_offset"]="24";
    config.values["game_rpc_frame_code_offset"]="32";config.values["game_rpc_frame_locals_offset"]="40";
    config.values["game_rpc_frame_current_function_offset"]="144";
    config.values["game_rpc_sample_auto_arm"]="1";config.values["game_rpc_sample_window_seconds"]="1";
    static ebon::rpc_samples::Capture capture;
    const auto folder=output/"rpc-binding";
    require(capture.install(memory,reflection,config,module,folder,status)&&status=="ready_press_f8"&&table[76]!=derived,
            "verified inherited PlayerState event override could not be installed");
    require(contents(folder/"game_rpc_sample_state.json").find("\"hooked_vtables\":1")!=std::string::npos,
            "shared PlayerState vtable was patched more than once");
    using Event=void(__fastcall*)(void*,void*,void*);
    alignas(8) std::array<std::uint8_t,32> data{};const std::int32_t messageId=7;std::memcpy(data.data(),&messageId,4);
    const auto invoke=[&](std::uint64_t function){reinterpret_cast<Event>(table[76])(&object,reinterpret_cast<void*>(function),data.data());};
    require(contents(folder/"game_rpc_sample_state.json").find("\"status\":\"armed\"")!=std::string::npos,
            "automatic startup sampling did not arm the window");
    invokeSamplerNative(&object,rpcAddress,data.data());capture.flush();
    require(contents(folder/"game_rpc_samples.jsonl").find("UFunction_native_parameters")!=std::string::npos,
            "direct native invocation bypassing ProcessEvent was not captured");
    Sleep(1100);capture.flush();
    require(contents(folder/"game_rpc_sample_state.json").find("\"status\":\"window_expired\"")!=std::string::npos,
            "automatic sampling window did not expire");
    invoke(rpcAddress);capture.flush();
    auto expiredSamples=contents(folder/"game_rpc_samples.jsonl");
    require(std::count(expiredSamples.begin(),expiredSamples.end(),'\n')==1,"expired sampler recorded a call");
    capture.arm();invoke(rpcAddress);invoke(rpcAddress+8);capture.flush();
    require(samplerOverrideCalls==3&&samplerRootCalls==3&&samplerLastObject==&object&&
            samplerLastFunction==reinterpret_cast<void*>(rpcAddress+8)&&samplerLastParams==data.data(),
            "sampler skipped the inherited override or changed forwarded arguments");
    auto samples=contents(folder/"game_rpc_samples.jsonl");
    require(std::count(samples.begin(),samples.end(),'\n')==2&&samples.find("\"message_id\":7")!=samples.npos,
            "dual entry sampling duplicated the same RPC invocation");
    capture.arm();invoke(rpcAddress);capture.flush();capture.reportStatus();
    int result=0;invokeSamplerNative(&object,rpcAddress,data.data(),&result);capture.flush();
    require(result==77,"native result pointer was not forwarded to the original function");
    alignas(8) std::array<std::uint8_t,64> fogData{};const wchar_t fogName[]=L"FogRecord_001";
    constexpr auto nameLength=static_cast<std::int32_t>(sizeof(fogName)/sizeof(wchar_t));
    const ebon::rpc_samples::ArrayHeader header{reinterpret_cast<std::uint64_t>(fogName),nameLength,nameLength};
    std::memcpy(fogData.data(),&header,sizeof(header));invokeSamplerNative(&object,fogAddress,fogData.data());capture.flush();
    const auto beforeRejects=contents(folder/"game_rpc_samples.jsonl");
    invokeSamplerNative(&object,rpcAddress,data.data(),nullptr,1);
    const std::int32_t badCount=-1;std::memcpy(data.data()+16,&badCount,4);
    invokeSamplerNative(&object,rpcAddress,data.data());Sleep(1100);capture.flush();
    require(contents(folder/"game_rpc_samples.jsonl")==beforeRejects&&samplerNativeCalls==8,
            "invalid native frames/parameters were recorded or the original function was not forwarded");
    const auto state=contents(folder/"game_rpc_sample_state.json");
    require(state.find("\"window_samples\":3")!=state.npos&&state.find("\"captured_total\":5")!=state.npos,
            "rearming did not preserve totals while resetting the window count");
    ebon::save(folder/"legacy_sampling_state.json",state);
    capture.arm();
    alignas(8) std::array<std::uint8_t,32> syncWrapper{};syncWrapper[0]=14;
    const std::uint8_t syncPayload[]={0xaa,0xbb,0xcc};
    const ebon::rpc_samples::SyncArray payloadHeader{reinterpret_cast<std::uint64_t>(syncPayload),3,3};
    std::memcpy(syncWrapper.data()+16,&payloadHeader,16);
    alignas(8) std::array<std::uint8_t,40> syncMessage{};
    const ebon::rpc_samples::SyncArray wrappersHeader{reinterpret_cast<std::uint64_t>(syncWrapper.data()),1,1};
    std::memcpy(syncMessage.data(),&wrappersHeader,16);std::int32_t messageType=21;
    std::memcpy(syncMessage.data()+24,&messageType,4);
    const auto syncInvoke=[&](std::uint64_t function,void* parameters){reinterpret_cast<Event>(table[76])(&object,reinterpret_cast<void*>(function),parameters);};
    syncInvoke(dataLayer.address,syncMessage.data());
    messageType=22;std::memcpy(syncMessage.data()+24,&messageType,4);
    const std::uint64_t actor=0x12345678;std::memcpy(syncMessage.data()+32,&actor,8);
    syncInvoke(common.address,syncMessage.data());
    alignas(8) std::array<std::uint8_t,96> containerBytes{};
    alignas(8) std::array<std::uint8_t,32> propertyBytes{};
    const ebon::rpc_samples::SyncArray propertiesHeader{reinterpret_cast<std::uint64_t>(propertyBytes.data()),1,1};
    std::memcpy(containerBytes.data()+8,&propertiesHeader,16);std::memcpy(containerBytes.data()+24,&actor,8);
    const ebon::rpc_samples::SyncName propertyName{1234,0},moduleName{5678,0};
    std::memcpy(containerBytes.data()+84,&moduleName,8);std::memcpy(propertyBytes.data(),&propertyName,8);
    std::memcpy(propertyBytes.data()+8,&payloadHeader,16);propertyBytes[24]=1;
    ebon::rpc_samples::SyncArray containerHeader{reinterpret_cast<std::uint64_t>(containerBytes.data()),1,1};
    syncInvoke(propertiesRpc.address,&containerHeader);capture.flush();
    std::string truncated;
    require(ebon::rpc_samples::captureReplicatedProperties(memory,reinterpret_cast<std::uint64_t>(&containerHeader),propertiesLayout,
            actor,propertiesRpc.address,999,1,2,0,nullptr,"fixture",truncated)&&truncated.find("\"truncated\":true")!=truncated.npos&&
            truncated.find("\"captured_payload_hex\":\"aabb\"")!=truncated.npos,"replicated-property payload budget was not enforced");
    ebon::save(folder/"replicated_truncated.json",truncated);
    const std::int32_t negative=-1;std::memcpy(propertyBytes.data()+16,&negative,4);
    const auto beforeInvalid=contents(folder/"game_rpc_samples.jsonl");
    syncInvoke(propertiesRpc.address,&containerHeader);Sleep(1100);capture.flush();
    require(contents(folder/"game_rpc_samples.jsonl")==beforeInvalid,"invalid replicated-property payload was recorded");
    const auto syncState=contents(folder/"game_rpc_sample_state.json");
    require(syncState.find("\"window_samples\":3")!=syncState.npos&&syncState.find("\"sync_samples_total\":3")!=syncState.npos&&
            syncState.find("\"window_parse_rejected\":1")!=syncState.npos&&syncState.find("\"captured_total\":8")!=syncState.npos,
            "observed sync routes did not capture data independently of the absent legacy targets");
    const auto log=contents(output/"rpc-binding.log");
    require(log.find("[F8] status=no_verified_playerstate_vtable")!=log.npos&&
            log.find("[F8] status=ready_press_f8")!=log.npos&&log.find("[F8] status=armed")!=log.npos&&
            log.find("[F8] status=capturing")!=log.npos,
            "console log omitted a sampler failure or state transition");
    ebon::console::setLogFile({});
}

class Fixture final:public ebon::MemoryReader {
    static constexpr std::uint64_t base=0x10000;
    std::vector<std::uint8_t> bytes_=std::vector<std::uint8_t>(0x1000000);
    std::map<std::string,std::uint32_t> names_;
    std::map<std::string,std::uint64_t> fieldClasses_;
    std::uint64_t nextField_=0x90000,nextString_=0x800000;
public:
    static constexpr std::uint64_t module=0x10000,pool=0x10200,array=0x10300;
    static constexpr std::uint64_t block=0x30000,chunks=0x40000,items=0x41000;
    static constexpr std::uint64_t tableObject=0xa0000,assetObject=0xa0200;
    static constexpr std::uint64_t tableMap=tableObject+48,rowItems=0xb0000;
    static constexpr std::uint64_t firstRow=0xc0000,secondRow=0xc0200;
    static constexpr std::uint64_t firstLookup=0xe0000,secondLookup=0xe0200;
    static constexpr std::uint64_t nestedType=0x80300;

    template<class T> void put(std::uint64_t address,T value) {
        require(address>=base&&address+sizeof(value)<=base+bytes_.size(),"fixture write outside memory");
        std::memcpy(bytes_.data()+address-base,&value,sizeof(value));
    }
    void putBytes(std::uint64_t address,const void* data,std::size_t size) {
        require(address>=base&&address+size<=base+bytes_.size(),"fixture byte write outside memory");
        std::memcpy(bytes_.data()+address-base,data,size);
    }
    bool copy(std::uint64_t address,void* output,std::size_t size)const override {
        if(!output||address<base||address+size<address||address+size>base+bytes_.size())return false;
        std::memcpy(output,bytes_.data()+address-base,size);return true;
    }
    std::vector<ebon::Region> regions(const ebon::Module&)const override{return {{module,0x1000,false,true}};}
    bool executable(std::uint64_t address)const override{return address>=0x10800&&address<0x11000;}
    void fname(std::uint64_t address,const std::string& name) {
        if(!names_.count(name)){
            std::uint32_t cursor=0;copy(pool+12,&cursor,sizeof(cursor));names_[name]=cursor/2;
            put(block+cursor,static_cast<std::uint16_t>(name.size()<<6));putBytes(block+cursor+2,name.data(),name.size());
            cursor+=static_cast<std::uint32_t>((2+name.size()+1)&~std::size_t(1));put(pool+12,cursor);
        }
        put(address,names_.at(name));put(address+4,std::uint32_t(0));
    }
    void fstring(std::uint64_t address,const std::wstring& value) {
        const auto count=static_cast<std::int32_t>(value.size()+1);
        put(address,ebon::resources::ArrayHeader{nextString_,count,count});
        putBytes(nextString_,value.c_str(),count*sizeof(wchar_t));
        nextString_+=(count*sizeof(wchar_t)+15)&~std::uint64_t(15);
    }
    void sparseMap(std::uint64_t address,std::uint64_t data,std::int32_t slots,
                   const std::vector<std::int32_t>& live,std::int32_t maxBits=128,
                   std::uint64_t externalBits=0) {
        put(address,ebon::resources::ArrayHeader{data,slots,slots});
        std::vector<std::uint32_t> flags((slots+31)/32);
        for(const auto index:live){require(index>=0&&index<slots,"invalid fixture live slot");flags[index/32]|=1u<<(index%32);}
        const auto storage=maxBits>128?externalBits:address+16;
        if(!flags.empty())putBytes(storage,flags.data(),flags.size()*4);
        if(maxBits>128)put(address+32,externalBits);
        const auto free=slots-static_cast<std::int32_t>(live.size());
        std::int32_t first=-1;
        if(free)for(std::int32_t n=0;n<slots;++n)if(std::find(live.begin(),live.end(),n)==live.end()){first=n;break;}
        put(address+40,slots);put(address+44,maxBits);put(address+48,first);put(address+52,free);
    }
    std::uint64_t descriptor(const std::string& type,std::int32_t size) {
        const auto field=nextField_;nextField_+=0x100;
        put(field+8,fieldClasses_.at(type));put(field+60,size);
        return field;
    }
    ebon::Property property(const std::string& name,const std::string& type,std::int32_t offset,std::int32_t size) {
        ebon::Property p;p.name=name;p.type=type;p.address=descriptor(type,size);
        p.offset=offset;p.element=size;p.array=1;
        return p;
    }
    void object(std::uint64_t address,std::uint64_t klass,std::int32_t index,const std::string& name) {
        put(address,std::uint64_t(0x10800));put(address+12,index);put(address+16,klass);fname(address+24,name);
    }
    Fixture() {
        const std::vector<std::string> strings={
            "None","ByteProperty","IntProperty","BoolProperty","ScriptStruct","Class",
            "Function","Enum","EExample","FExample","Actor","Health","Visible","Extra",
            "StrProperty","ArrayProperty","StructProperty","MapProperty","NameProperty","TextProperty",
            "CharacterA","CharacterB","Level","Rarity","DT_CharacterFashion","LotteryRuntime","ST_Character",
            "ObjectProperty","DoubleProperty","MapFixture","PersistentLevel","PlacedNPC","SceneRoot",
            "LuaFixture","Default__PlacedNPC","EvalFixture","QuestRuntime"
        };
        std::uint32_t cursor=0;
        for(const auto& text:strings){
            names_[text]=cursor/2;put(block+cursor,static_cast<std::uint16_t>(text.size()<<6));
            putBytes(block+cursor+2,text.data(),text.size());
            cursor+=static_cast<std::uint32_t>((2+text.size()+1)&~std::size_t(1));
        }
        put(pool+16,block);put(pool+12,cursor);put(pool+8,std::uint32_t(0));
        put(array+16,chunks);put(chunks,items);put(array+36,std::int32_t(6));
        put(array+32,std::int32_t(65536));put(array+44,std::int32_t(1));put(array+40,std::int32_t(1));
        put(std::uint64_t(0x10800),std::uint64_t(0x10840));
        const std::uint64_t cls=0x60000,str=0x60100,fn=0x60200,en=0x60300,example=0x60400,actor=0x60500;
        struct Entry {std::uint64_t address,klass;const char* name;};
        const Entry seed[]={{cls,cls,"Class"},{str,cls,"ScriptStruct"},{fn,cls,"Function"},
                            {en,cls,"Enum"},{example,en,"EExample"},{actor,str,"Actor"}};
        for(int n=0;n<6;++n){
            put(items+std::uint64_t(n)*24,seed[n].address);
            object(seed[n].address,seed[n].klass,n,seed[n].name);
            put(seed[n].address+88,std::int32_t(16));
        }
        std::uint64_t fieldClass=0x50000;
        for(const auto* type:{"IntProperty","BoolProperty","StrProperty","ArrayProperty","StructProperty","MapProperty","NameProperty","TextProperty","ObjectProperty","DoubleProperty","SoftObjectProperty","SoftClassProperty","Utf8StrProperty"}){
            fieldClasses_[type]=fieldClass;fname(fieldClass,type);fieldClass+=0x100;
        }
        const auto health=std::uint64_t(0x51000);
        put(actor+80,health);put(health+8,fieldClasses_.at("IntProperty"));fname(health+40,"Health");
        put(health+56,std::int32_t(1));put(health+60,std::int32_t(4));put(health+76,std::int32_t(0));
    }
    void resources(ebon::Reflection& reflection) {
        ebon::Type table;table.kind="class";table.name="DataTable";table.path="/Script/Engine.DataTable";
        table.address=0x80000;table.size=176;
        ebon::Property rowStruct;rowStruct.name="RowStruct";rowStruct.offset=40;rowStruct.element=8;rowStruct.array=1;
        table.fields={rowStruct};
        ebon::Type dataAsset;dataAsset.kind="class";dataAsset.name="DataAsset";dataAsset.path="/Script/Engine.DataAsset";
        dataAsset.address=0x80100;dataAsset.size=48;
        dataAsset.fields={property("InheritedSetting","IntProperty",40,4)};
        ebon::Type emptyBase;emptyBase.kind="class";emptyBase.name="EmptyConfigBase";
        emptyBase.path="/Script/Test.EmptyConfigBase";emptyBase.address=0x80500;emptyBase.size=48;emptyBase.super=dataAsset.path;
        ebon::Type manager;manager.kind="class";manager.name="HTLotteryManager";manager.path="/Script/HTGame.HTLotteryManager";
        manager.address=0x80200;manager.size=80;manager.super=emptyBase.path;
        manager.fields={property("PoolGuaranteeCount","IntProperty",48,4),property("FashionName","StrProperty",56,16)};
        ebon::Type nested;nested.kind="struct";nested.name="CharacterRank";nested.path="/Script/Test.CharacterRank";
        nested.address=nestedType;nested.size=8;nested.alignment=4;
        nested.fields={property("Rank","IntProperty",0,4),property("Ready","BoolProperty",4,1)};
        const unsigned char boolean[]={1,0,4,4};
        putBytes(nested.fields[1].address+120,boolean,4);
        ebon::Type row;row.kind="struct";row.name="CharacterFashionRow";row.path="/Script/Test.CharacterFashionRow";
        row.address=0x80400;row.size=160;row.alignment=8;
        row.fields={property("HitPoints","IntProperty",0,4),property("Title","StrProperty",8,16),
                    property("Enabled","BoolProperty",24,1),property("Scores","ArrayProperty",32,16),
                    property("Nested","StructProperty",48,8),property("Lookup","MapProperty",56,80),
                    property("UnverifiedText","TextProperty",136,16)};
        putBytes(row.fields[2].address+120,boolean,4);
        const auto arrayInner=descriptor("IntProperty",4);
        put(arrayInner+16,row.fields[3].address);put(row.fields[3].address+120,arrayInner);
        put(row.fields[4].address+120,nested.address);
        put(row.fields[5].address+120,descriptor("NameProperty",8));
        put(row.fields[5].address+128,descriptor("IntProperty",4));
        reflection.types={table,dataAsset,emptyBase,manager,nested,row};
        reflection.objects={
            {100,tableObject,table.address,"DT_CharacterFashion","DataTable","/Game/DataTable/DT_CharacterFashion.DT_CharacterFashion"},
            {101,assetObject,manager.address,"LotteryRuntime","HTLotteryManager","/Engine/Transient.GameInstance.HTLotteryManager.LotteryRuntime"}
        };
        object(tableObject,table.address,100,"DT_CharacterFashion");put(tableObject+40,row.address);
        object(assetObject,manager.address,101,"LotteryRuntime");put(assetObject+40,std::int32_t(314));put(assetObject+48,std::int32_t(90));fstring(assetObject+56,L"活动时装");
        sparseMap(tableMap,rowItems,3,{0,2});
        fname(rowItems,"CharacterA");put(rowItems+8,firstRow);
        // The freed row's key/value deliberately contain unreadable garbage.
        put(rowItems+24,std::uint64_t(0xffffffffffffffff));put(rowItems+32,std::uint64_t(1));
        fname(rowItems+48,"CharacterB");put(rowItems+56,secondRow);
        for(int n=0;n<2;++n){
            const auto address=n?secondRow:firstRow;
            const auto lookup=n?secondLookup:firstLookup;
            const auto scores=std::uint64_t(0xf0000+n*0x100);
            put(address,std::int32_t(n?456:123));fstring(address+8,n?L"角色二":L"角色一");
            put(address+24,std::uint8_t(n?0:4));
            put(address+32,ebon::resources::ArrayHeader{scores,2,2});put(scores,std::int32_t(7+n));put(scores+4,std::int32_t(9+n));
            put(address+48,std::int32_t(5+n));put(address+52,std::uint8_t(4));
            sparseMap(address+56,lookup,3,{0,2});
            fname(lookup,"Level");put(lookup+8,std::int32_t(10+n));
            put(lookup+20,std::uint64_t(0xffffffffffffffff));
            fname(lookup+40,"Rarity");put(lookup+48,std::int32_t(5));
            put(address+136,std::uint64_t(0x180000+n*0x100));
            fstring(0x180000+n*0x100+16,L"candidate only");
        }
    }
    void stringResource(ebon::Reflection& reflection) {
        ebon::Type type;type.kind="class";type.name="StringTable";type.path="/Script/Engine.StringTable";
        type.address=0x80500;type.size=64;reflection.types.push_back(type);
        reflection.objects={{102,0xa0400,type.address,"ST_Character","StringTable","/Game/Text/ST_Character.ST_Character"}};
        object(0xa0400,type.address,102,"ST_Character");put(std::uint64_t(0xa0428),std::uint64_t(0x1c0000));
        sparseMap(0x1c0040,0x1d0000,3,{0,2});
        fstring(0x1d0000,L"CharacterA.Name");put(std::uint64_t(0x1d0010),std::uint64_t(0x1e0000));
        fstring(0x1e0008,L"角色名称");
        fstring(0x1d0050,L"CharacterA.Description");put(std::uint64_t(0x1d0060),std::uint64_t(0x1e0100));
        fstring(0x1e0108,L"角色说明");
        put(std::uint64_t(0x1d0028),std::uint64_t(0xffffffffffffffff));
    }
    void featureResources(ebon::Reflection& reflection) {
        const auto type=[](std::uint64_t address,const char* path,int size,const char* super="",const char* kind="class"){
            ebon::Type t;t.address=address;t.path=path;t.name=t.path.substr(t.path.rfind('.')+1);t.kind=kind;t.size=size;t.alignment=8;t.super=super;return t;
        };
        auto dataAsset=type(0x84000,"/Script/Engine.DataAsset",40);
        auto table=type(0x84100,"/Script/Engine.DataTable",176);
        table.fields={property("RowStruct","ObjectProperty",40,8)};
        auto top=type(0x84200,"/Script/CoreUObject.TopLevelAssetPath",16,"","struct");
        top.fields={property("PackageName","NameProperty",0,8),property("AssetName","NameProperty",8,8)};
        auto path=type(0x84300,"/Script/CoreUObject.SoftObjectPath",32,"","struct");
        path.fields={property("AssetPath","StructProperty",0,16),property("SubPathString","Utf8StrProperty",16,16)};
        path.fields[0].target=top.path;put(path.fields[0].address+120,top.address);
        auto asset=type(0x84400,"/Script/HTGame.SystematicGameFeaturesDataAsset",200,dataAsset.path.c_str());
        asset.fields={property("DataTables","MapProperty",40,80),property("LoadedDataTables","MapProperty",120,80)};
        put(asset.fields[0].address+120,descriptor("NameProperty",8));put(asset.fields[0].address+128,descriptor("SoftObjectProperty",40));
        put(asset.fields[1].address+120,descriptor("NameProperty",8));put(asset.fields[1].address+128,descriptor("ObjectProperty",8));
        auto action=type(0x84500,"/Script/Test.OpenMenuAction",8,"","struct");
        action.fields={property("UINameToOpen","NameProperty",0,8)};
        auto instanced=type(0x84600,"/Script/CoreUObject.InstancedStruct",16,"","struct");
        auto row=type(0x84700,"/Script/HTGame.SystematicGameFeaturesDefinitions",64,"","struct");
        row.fields={property("ID","NameProperty",0,8),property("Requires","ArrayProperty",8,16),
                    property("Actions","ArrayProperty",24,16),property("UINameToOpen","NameProperty",40,8),
                    property("QuickMenuIndex_Keyboard","IntProperty",48,4),property("QuickMenuIndex_Gamepad","IntProperty",52,4),
                    property("bEnableQuickMenuOption","BoolProperty",56,1)};
        const auto inner=descriptor("NameProperty",8);put(inner+16,row.fields[1].address);put(row.fields[1].address+120,inner);
        const auto actionInner=descriptor("StructProperty",16);put(actionInner+16,row.fields[2].address);
        put(actionInner+120,instanced.address);put(row.fields[2].address+120,actionInner);
        const unsigned char boolean[]={1,0,1,255};putBytes(row.fields[6].address+120,boolean,4);
        auto actorComponent=type(0x84800,"/Script/Engine.ActorComponent",40);
        auto component=type(0x84900,"/Script/HTGame.SystematicGameFeaturesComponent",56,actorComponent.path.c_str());
        component.fields={property("ActivatedFeatureIds","ArrayProperty",40,16)};
        const auto ids=descriptor("NameProperty",8);put(ids+16,component.fields[0].address);put(component.fields[0].address+120,ids);
        auto derived=type(0x84a00,"/Game/UI/BP_Menu.BP_Menu_C",56,component.path.c_str());
        auto player=type(0x84b00,"/Script/HTGame.HTPlayerState",40);
        auto button=type(0x84c00,"/Script/HTGame.HTUI_MenuButton",56);
        button.fields={property("GameFeatureId","NameProperty",40,8),property("FunctionUnlockId","NameProperty",48,8)};
        auto combat=type(0x84d00,"/Script/HTGame.CombatConfig",120,dataAsset.path.c_str());
        combat.fields={property("CharacterBlueprint","SoftClassProperty",40,40),property("Skill","SoftObjectProperty",80,40)};
        auto character=type(0x84e00,"/Game/Characters/BP_Hero.BP_Hero_C",48);
        character.fields={property("Attack","IntProperty",40,4)};
        reflection.types={dataAsset,table,top,path,asset,action,instanced,row,actorComponent,component,derived,player,button,combat,character};
        reflection.objects={
            {300,0xa2000,asset.address,"DA_SystematicGameFeatures","SystematicGameFeaturesDataAsset","/Game/UI/DA_SystematicGameFeatures.DA_SystematicGameFeatures"},
            {301,0xa2200,table.address,"DT_SystematicGameFeatures","DataTable","/Game/UI/DT_SystematicGameFeatures.DT_SystematicGameFeatures"},
            {302,0xa2400,table.address,"FeatureExtra","DataTable","/Game/UI/FeatureExtra.FeatureExtra"},
            {303,0xa2600,derived.address,"MenuComponent","BP_Menu_C","/Engine/Transient.Player.MenuComponent"},
            {304,0xa2800,player.address,"Player","HTPlayerState","/Engine/Transient.Player"},
            {305,0xa2a00,button.address,"Button","HTUI_MenuButton","/Engine/Transient.Player.Button"},
            {306,0xa2c00,derived.address,"Default__MenuComponent","BP_Menu_C","/Game/UI/BP_Menu.Default__MenuComponent"},
            {307,0xa2e00,combat.address,"DA_CharacterCombat","CombatConfig","/Game/Characters/DA_CharacterCombat.DA_CharacterCombat"},
            {308,0x84e00,0x60000,"BP_Hero_C","BlueprintGeneratedClass",character.path},
            {309,0xa3000,character.address,"Default__BP_Hero_C","BP_Hero_C","/Game/Characters/BP_Hero.Default__BP_Hero_C"},
            {310,0xa3200,dataAsset.address,"DA_Skill","DataAsset","/Game/Skills/DA_Skill.DA_Skill"}
        };
        for(const auto& o:reflection.objects)object(o.address,o.klass,o.index,o.name);
        put(std::uint64_t(0xa2620),std::uint64_t(0xa2800));put(std::uint64_t(0xa2608),std::uint32_t(0x40000));
        put(std::uint64_t(0xa2c08),std::uint32_t(0x10));put(std::uint64_t(0xa3008),std::uint32_t(0x10));
        put(std::uint64_t(0xa3028),std::int32_t(900));
        const auto soft=[&](std::uint64_t address,const char* package,const char* name){
            fname(address+8,package);fname(address+16,name);put(address+24,ebon::resources::ArrayHeader{});
        };
        sparseMap(0xa2028,0x110000,3,{0,1,2});
        fname(0x110000,"Main");soft(0x110008,"/Game/UI/DT_SystematicGameFeatures","DT_SystematicGameFeatures");
        fname(0x110038,"Extra");soft(0x110040,"/Game/UI/FeatureExtra","FeatureExtra");
        fname(0x110070,"Unloaded");soft(0x110078,"/Game/UI/NotLoaded","NotLoaded");
        sparseMap(0xa2078,0x110200,1,{0});fname(0x110200,"Extra");put(std::uint64_t(0x110208),std::uint64_t(0xa2400));
        for(unsigned i=0;i<2;++i){
            const auto t=0xa2200+i*0x200,rowData=0x120000+i*0x200,itemsData=0x121000+i*0x200;
            put(std::uint64_t(t+40),row.address);sparseMap(t+48,itemsData,1,{0});fname(itemsData,i?"Extra":"Main");put(std::uint64_t(itemsData+8),std::uint64_t(rowData));
            fname(rowData,i?"Extra":"Main");put(rowData+8,ebon::resources::ArrayHeader{0x122000,1,1});fname(0x122000,"Unlock1");
            put(rowData+24,ebon::resources::ArrayHeader{0x122100,1,1});put(std::uint64_t(0x122100),action.address);put(std::uint64_t(0x122108),std::uint64_t(0x122200));fname(0x122200,"MainForm");
            fname(rowData+40,"MainForm");put(rowData+48,std::int32_t(2));put(rowData+52,std::int32_t(3));put(rowData+56,std::uint8_t(1));
        }
        put(std::uint64_t(0xa2628),ebon::resources::ArrayHeader{0x123000,1,1});fname(0x123000,"Main");
        fname(0xa2a28,"Main");fname(0xa2a30,"Unlock1");
        soft(0xa2e28,"/Game/Characters/BP_Hero","BP_Hero_C");soft(0xa2e50,"/Game/Skills/DA_Skill","DA_Skill");
    }
    void networkFeatureResources(ebon::Reflection& reflection,const std::filesystem::path& output){
        const std::string target="/Script/HTGame.SystematicGameFeaturesComponent";
        std::map<std::uint64_t,ebon::NetworkRuntimeClass> classes={
            {0x84000,{0x84000,1,"/Script/CoreUObject.Object",""}},
            {0x84100,{0x84100,2,"/Script/HTGame.HTSystematicComponent","/Script/CoreUObject.Object"}},
            {0x84200,{0x84200,3,target,"/Script/HTGame.HTSystematicComponent"}}
        };
        const std::array<const char*,3> names={"ClientGameFeaturesToReadyForActivate","ClientOnSystematicGameFeatureActivated","ServerSetupSystematicGameFeatures"};
        std::map<std::uint64_t,ebon::NetworkRuntimeField> fields;
        reflection.types.clear();
        for(const auto& pair:classes){ebon::Type t;t.address=pair.first;t.kind="class";t.path=pair.second.path;t.super=pair.second.super;reflection.types.push_back(t);}
        for(unsigned i=0;i<3;++i){
            const auto address=0x151000ull+i*0x100;fields[address]={address,target+"."+names[i],target,0,true,1,1};
            ebon::Type t;t.kind="function";t.name=names[i];t.path=target+"."+names[i];t.address=address;t.flags=i==2?0x220cc0:0x1020cc0;reflection.types.push_back(t);
        }
        const std::function<void()> check=[](){};
        ebon::NetworkRuntimeDecoder decoder(*this,classes,fields,check);
        const auto cache=[&](std::uint64_t address,int index,std::uint64_t parent,int count){
            put(address,std::int32_t(0));put(address+8,parent);put(address+16,std::int32_t(index));put(address+20,std::int32_t(7));
            put(address+32,ebon::resources::ArrayHeader{count?0x133000ull:0ull,count,count});
        };
        cache(0x130000,1,0,0);cache(0x130100,2,0x130000,0);cache(0x130200,3,0x130100,3);
        for(unsigned i=0;i<3;++i){
            put(0x133000+i*24,std::uint64_t(0x151000+i*0x100));put(0x133008+i*24,std::int32_t(i));put(0x13300c+i*24,std::uint32_t(100+i));
        }
        put(std::uint64_t(0x134000),std::uint64_t(0x130200));
        const auto result=decoder.scan({0x134000},{module,0x1000,L"fixture",L"fixture"},2000);
        require(result.cacheRecords.size()==3,"target cache did not retain the two empty ancestor caches");
        require(result.cacheRecords.front().fieldsBase==0&&result.cacheRecords.front().fieldCount==3,"zero-based real field indices were rejected");
        put(std::uint64_t(0x133038),std::int32_t(-1));
        require(decoder.decodeCache(0x130200).empty(),"sentinel field index was accepted");
        put(std::uint64_t(0x133038),std::int32_t(2));
        reflection.runtimeNetwork=result;reflection.networkCacheCandidates=result.caches;
        reflection.objects.clear();
        for(const auto& pair:classes)reflection.objects.push_back({pair.second.index,pair.first,0x60000,"Class","Class",pair.second.path});
        reflection.objects.push_back({4,0x134000,0x60000,"Driver","IpNetDriver","/Engine/Transient.Driver"});
        std::filesystem::create_directories(output);
        ebon::save(output/"network_metadata.json",reflection.networkJson());
        reflection.write(output/"reflection",true);
    }
    void miniMapResources(ebon::Reflection& reflection) {
        const auto type=[](std::uint64_t address,const char* path,int size,const char* super="",const char* kind="class"){
            ebon::Type t;t.address=address;t.path=path;t.name=t.path.substr(t.path.rfind('.')+1);t.kind=kind;t.size=size;t.alignment=8;t.super=super;return t;
        };
        auto dataAsset=type(0x85000,"/Script/Engine.DataAsset",40);
        auto table=type(0x85100,"/Script/Engine.DataTable",176);
        table.fields={property("RowStruct","ObjectProperty",40,8)};
        auto asset=type(0x85200,"/Script/HTGame.MiniMapDataAsset",48,dataAsset.path.c_str());
        asset.fields={property("MiniMapLevelFogBlockDataTable","ObjectProperty",40,8)};
        auto row=type(0x85300,"/Script/HTGame.MiniMapFogBlockData",48,"","struct");
        row.fields={property("BelongsLevel","StrProperty",8,16),
                    property("BelongsGameplayAreaID","NameProperty",24,8),
                    property("DefaultUnlock","BoolProperty",32,1)};
        const unsigned char boolean[]={1,0,1,255};putBytes(row.fields[2].address+120,boolean,4);
        auto fogRecord=type(0x85400,"/Script/HTGame.MiniMapFogRecord",8,"","struct");
        fogRecord.fields={property("FogID","IntProperty",0,4)};
        reflection.types={dataAsset,table,asset,row,fogRecord};
        constexpr std::uint64_t miniTableObject=0xa3600,miniAssetObject=0xa3800,
            fogRecordTableObject=0xa3a00,miniRowItems=0x130000,miniRowData=0x140000,
            fogRecordItems=0x170000,fogRecordData=0x180000;
        reflection.objects={
            {400,miniTableObject,table.address,"DT_MiniMapFogBlock","DataTable","/Game/DataTable/DT_MiniMapFogBlock.DT_MiniMapFogBlock"},
            {402,fogRecordTableObject,table.address,"DT_MiniMapFogRecord","DataTable","/Game/DataTable/MiniMap/DT_MiniMapFogRecord.DT_MiniMapFogRecord"},
            {401,miniAssetObject,asset.address,"Default__MiniMapDataAsset","MiniMapDataAsset","/Script/HTGame.Default__MiniMapDataAsset"}
        };
        for(const auto& objectRecord:reflection.objects)object(objectRecord.address,objectRecord.klass,objectRecord.index,objectRecord.name);
        put(miniTableObject+40,row.address);put(miniAssetObject+40,miniTableObject);
        put(fogRecordTableObject+40,fogRecord.address);put(fogRecordData,std::int32_t(77));
        sparseMap(miniTableObject+48,miniRowItems,1,{0});fname(miniRowItems,"FogBlock_001");put(miniRowItems+8,miniRowData);
        sparseMap(fogRecordTableObject+48,fogRecordItems,1,{0});fname(fogRecordItems,"FogRecord_001");put(fogRecordItems+8,fogRecordData);
        fstring(miniRowData+8,L"XL_map_bigworld");fname(miniRowData+24,"GameplayArea_01");put(miniRowData+32,std::uint8_t(1));
    }
    void sceneResources(ebon::Reflection& reflection) {
        ebon::Type world;world.kind="class";world.path="/Script/Engine.World";world.address=0x81000;world.size=48;
        world.fields={property("PersistentLevel","ObjectProperty",40,8)};
        ebon::Type level;level.kind="class";level.path="/Script/Engine.Level";level.address=0x81200;level.size=48;
        level.fields={property("OwningWorld","ObjectProperty",40,8)};
        ebon::Type actor;actor.kind="class";actor.path="/Script/Engine.Actor";actor.address=0x81400;actor.size=56;
        actor.fields={property("SpawnId","IntProperty",40,4),property("RootComponent","ObjectProperty",48,8)};
        ebon::Type componentBase;componentBase.kind="class";componentBase.path="/Script/Engine.ActorComponent";componentBase.address=0x81600;componentBase.size=40;
        ebon::Type vector;vector.kind="struct";vector.path="/Script/CoreUObject.Vector";vector.address=0x81800;vector.size=24;vector.alignment=8;
        vector.fields={property("X","DoubleProperty",0,8),property("Y","DoubleProperty",8,8),property("Z","DoubleProperty",16,8)};
        ebon::Type component;component.kind="class";component.path="/Script/Engine.SceneComponent";component.address=0x81a00;component.size=64;component.super=componentBase.path;
        component.fields={property("RelativeLocation","StructProperty",40,24)};
        put(component.fields[0].address+120,vector.address);
        ebon::Type lua;lua.kind="class";lua.path="/Script/Test.LuaAsset";lua.address=0x81c00;lua.size=56;
        lua.fields={property("Source","StrProperty",40,16)};
        ebon::Type evaluator;evaluator.kind="class";evaluator.path="/Script/Test.EvaluationEngine";evaluator.address=0x81e00;evaluator.size=40;
        ebon::Type quest;quest.kind="class";quest.path="/Script/HTGame.HTQuestManager";quest.address=0x82000;quest.size=48;
        quest.fields={property("CurrentQuestId","IntProperty",40,4)};
        reflection.types={world,level,actor,componentBase,vector,component,lua,evaluator,quest};
        reflection.objects={
            {200,0xa0800,world.address,"MapFixture","World","/Game/Maps/MapFixture.MapFixture"},
            {201,0xa0a00,level.address,"PersistentLevel","Level","/Game/Maps/MapFixture.MapFixture.PersistentLevel"},
            {202,0xa0c00,actor.address,"PlacedNPC","Actor","/Game/Maps/MapFixture.MapFixture.PersistentLevel.PlacedNPC"},
            {203,0xa0e00,component.address,"SceneRoot","SceneComponent","/Game/Maps/MapFixture.MapFixture.PersistentLevel.PlacedNPC.SceneRoot"},
            {204,0xa1000,lua.address,"LuaFixture","LuaAsset","/Game/Lua/LuaFixture.LuaFixture"},
            {205,0xa1200,actor.address,"Default__PlacedNPC","Actor","/Script/Engine.Default__PlacedNPC"},
            {206,0xa1400,evaluator.address,"EvalFixture","EvaluationEngine","/Engine/Transient.EvalFixture"},
            {207,0xa1600,quest.address,"QuestRuntime","HTQuestManager","/Engine/Transient.QuestRuntime"}
        };
        for(const auto& o:reflection.objects)object(o.address,o.klass,o.index,o.name);
        put(std::uint64_t(0xa0828),std::uint64_t(0xa0a00));
        put(std::uint64_t(0xa0a20),std::uint64_t(0xa0800));put(std::uint64_t(0xa0a28),std::uint64_t(0xa0800));
        put(std::uint64_t(0xa0c20),std::uint64_t(0xa0a00));put(std::uint64_t(0xa0c28),std::int32_t(777));put(std::uint64_t(0xa0c30),std::uint64_t(0xa0e00));
        put(std::uint64_t(0xa0e20),std::uint64_t(0xa0c00));
        put(std::uint64_t(0xa0e28),1.25);put(std::uint64_t(0xa0e30),-2.5);put(std::uint64_t(0xa0e38),3.75);
        fstring(0xa1028,L"return { map = '测试地图' }");
        put(std::uint64_t(0xa1228),std::int32_t(999));
        put(std::uint64_t(0xa1628),std::int32_t(5150));
    }
};

void checkSparse(Fixture& memory,ebon::Reflection& reflection,const ebon::Config& config,const std::filesystem::path& output) {
    ebon::resources::Dumper dumper(memory,reflection,config,output);
    ebon::resources::SparseView view;
    require(dumper.sparse(Fixture::tableMap,view)&&view.live==std::vector<std::int32_t>({0,2}),"sparse row map did not preserve holes");
    constexpr std::uint64_t sparse=0x190000,bits=0x1a0000;
    memory.sparseMap(sparse,0x1b0000,150,{0,65,149},160,bits);
    require(dumper.sparse(sparse,view)&&view.live==std::vector<std::int32_t>({0,65,149}),"external allocation bitmap was not read");
    memory.put(bits,std::uint32_t(3));
    require(!dumper.sparse(sparse,view),"corrupt bitmap population was accepted");
    memory.sparseMap(sparse,0x1b0000,150,{0,65,149},160,bits);
    memory.put(sparse+48,std::int32_t(150));
    require(!dumper.sparse(sparse,view),"invalid first-free index was accepted");
    memory.sparseMap(sparse,0x1b0000,150,{0,65,149},160,bits);
    memory.put(sparse+48,std::int32_t(0));
    require(!dumper.sparse(sparse,view),"allocated first-free index was accepted");
    memory.sparseMap(sparse,0x1b0000,150,{0,65,149},160,bits);
    memory.put(sparse+32,std::uint64_t(1));
    require(!dumper.sparse(sparse,view),"unreadable external bitmap was accepted");
}

int run(const std::filesystem::path& output) {
    Fixture memory;
    ebon::Config config;config.values={{"module","HTGame.exe"},{"name_pool_rva","0x200"},{"object_array_rva","0x300"}};
    ebon::Reflection reflection(memory,config,{Fixture::module,0x1000,L"HTGame.exe",L"synthetic resources"});
    require(reflection.collect(),"seed reflection fixture failed validation");
    reflection.moduleSha256="synthetic-resource-self-test";
    memory.resources(reflection);
    checkSparse(memory,reflection,config,output/"sparse-probe");
    {
        ebon::resources::Dumper dumper(memory,reflection,config,output/"complete");
        require(dumper.run(),"resource fixture capture failed");
    }
    const auto state=contents(output/"complete"/"resource_state.json");
    require(state.find("\"success\":true")!=state.npos&&state.find("\"rows\":2")!=state.npos&&
            state.find("\"resources\":2")!=state.npos,"resource counts or success were incorrect");
    const auto table=contents(output/"complete"/"data"/"DataTable"/"DT_CharacterFashion.jsonl");
    for(const auto* expected:{"\"row\":\"CharacterA\"","\"row\":\"CharacterB\"","\"HitPoints\":123",
                             "\"HitPoints\":456","\"Title\":\"角色一\"","\"Enabled\":true","\"Enabled\":false",
                             "\"Scores\":[7,9]","\"Nested\":{\"Rank\":5,\"Ready\":true}",
                             "\"Lookup\":[{\"key\":\"Level\",\"value\":10},{\"key\":\"Rarity\",\"value\":5}]",
                             "FText native layout unverified","candidate only"})
        require(table.find(expected)!=table.npos,"actual table value was missing or incorrect");
    require(table.find("invalid FName")==table.npos,"a free sparse slot was serialized as a row");
    const auto asset=contents(output/"complete"/"data"/"Engine"/"Transient.GameInstance.HTLotteryManager.LotteryRuntime.jsonl");
    require(asset.find("/Engine/Transient.")!=asset.npos&&asset.find("\"PoolGuaranteeCount\":90")!=asset.npos&&
            asset.find("\"is_default\":false")!=asset.npos,"transient DataAsset values were missing");
    require(asset.find("\"InheritedSetting\":314")!=asset.npos,"empty intermediate class lost ancestor values");
    for(const auto& category:ebon::resources::categoryNames())
        require(std::filesystem::exists(output/"complete"/category/"resources.jsonl"),"category index directory is missing");
    require(contents(output/"complete"/"characters"/"resources.jsonl").find("DataTable/DT_CharacterFashion.jsonl")!=std::string::npos,"character table routing failed");
    require(contents(output/"complete"/"gacha"/"resources.jsonl").find("Engine/Transient.GameInstance.HTLotteryManager.LotteryRuntime.jsonl")!=std::string::npos,"transient lottery routing failed");
    require(contents(output/"complete"/"character_skins"/"resources.jsonl").find("DataTable/DT_CharacterFashion.jsonl")!=std::string::npos,"fashion table routing failed");
    require(contents(output/"complete"/"characters"/"data"/"DataTable"/"DT_CharacterFashion.jsonl")==table,"category table body differs from canonical values");
    require(contents(output/"complete"/"character_skins"/"data"/"DataTable"/"DT_CharacterFashion.jsonl")==table,"second category table body was missing");
    require(contents(output/"complete"/"gacha"/"data"/"Engine"/"Transient.GameInstance.HTLotteryManager.LotteryRuntime.jsonl")==asset,"category asset body was missing");
    const auto gachaIndex=contents(output/"complete"/"gacha"/"resources.jsonl");
    require(gachaIndex.find("\"file\":\"gacha/data/Engine/Transient.GameInstance.HTLotteryManager.LotteryRuntime.jsonl\"")!=gachaIndex.npos&&
            gachaIndex.find("\"values\"")==gachaIndex.npos,"category index embeds values or points to the wrong body");
    {
        const auto conflict=output/"body-conflict";
        std::filesystem::create_directories(conflict/"characters"/"data"/"DataTable");
        ebon::save(conflict/"characters"/"data"/"DataTable"/"DT_CharacterFashion.jsonl","{\"keep\":\"existing body\"}\n");
        ebon::resources::Dumper dumper(memory,reflection,config,conflict);
        require(!dumper.run(),"existing category body was overwritten");
        require(contents(conflict/"characters"/"data"/"DataTable"/"DT_CharacterFashion.jsonl")=="{\"keep\":\"existing body\"}\n","body conflict changed existing bytes");
    }
    const auto textValues=contents(output/"complete"/"localization"/"text_values.jsonl");
    require(textValues.find("角色一")!=textValues.npos&&textValues.find("活动时装")!=textValues.npos&&
            textValues.find("FText native layout unverified")!=textValues.npos,"localization value summary was missing strings or unresolved text");
    reflection.errors.push_back("synthetic transient object changed during collection");
    {
        ebon::resources::Dumper dumper(memory,reflection,config,output/"partial-reflection");
        require(dumper.run(),"a usable partial reflection inventory was rejected");
    }
    const auto partialState=contents(output/"partial-reflection"/"resource_state.json");
    require(partialState.find("\"status\":\"complete_with_unresolved\"")!=partialState.npos&&
            partialState.find("\"success\":true")!=partialState.npos&&
            contents(output/"partial-reflection"/"unresolved.jsonl").find("synthetic transient object changed")!=std::string::npos,
            "partial reflection diagnostics were not retained with usable resources");
    reflection.errors.pop_back();
    {
        auto limited=config;limited.values["resource_max_values"]="2";
        ebon::resources::Dumper dumper(memory,reflection,limited,output/"budget");
        require(!dumper.run(),"exhausted resource value budget was accepted");
    }
    const auto failed=contents(output/"budget"/"resource_state.json");
    require(failed.find("\"status\":\"failed\"")!=failed.npos&&failed.find("\"success\":false")!=failed.npos&&
             failed.find("value budget exceeded")!=failed.npos,"budget exhaustion did not record a failed state");
    {
        auto limited=config;limited.values["resource_max_output_mb"]="1";
        auto broad=reflection;
        broad.objects[1].path="/Engine/Transient.CharacterLotteryQuestFashionMapConfigLuaAsset";
        memory.fstring(Fixture::assetObject+56,std::wstring(60000,L'界'));
        ebon::resources::Dumper dumper(memory,broad,limited,output/"body-byte-budget");
        require(!dumper.run(),"category body copies escaped the output byte budget");
        require(contents(output/"body-byte-budget"/"resource_state.json").find("category payload byte budget exceeded")!=std::string::npos,"category copy budget failure was not reported");
        memory.fstring(Fixture::assetObject+56,L"活动时装");
    }
    memory.put(Fixture::tableMap+16,std::uint32_t(7));
    {
        ebon::resources::Dumper dumper(memory,reflection,config,output/"corrupt");
        require(dumper.run(),"a bad table prevented the independent valid asset capture");
    }
    require(!std::filesystem::exists(output/"corrupt"/"data"/"DataTable"/"DT_CharacterFashion.jsonl"),"corrupt row map produced a table file");
    require(contents(output/"corrupt"/"unresolved.jsonl").find("RowMap layout not verified")!=std::string::npos,"corrupt table diagnostics were missing");
    memory.stringResource(reflection);
    {
        ebon::resources::Dumper dumper(memory,reflection,config,output/"text-candidates");
        require(dumper.run(),"validated StringTable candidate fixture failed");
    }
    const auto text=contents(output/"text-candidates"/"data"/"Text"/"ST_Character.jsonl");
    require(text.find("\"translations_verified\":false")!=text.npos&&text.find("inferred_candidates")!=text.npos&&
            text.find("\"key\":\"CharacterA.Name\"")!=text.npos&&text.find("角色名称")!=text.npos&&
            text.find("角色说明")!=text.npos,"StringTable candidates were missing or claimed verified translations");
    require(contents(output/"text-candidates"/"resource_state.json").find("\"text_entries\":2")!=std::string::npos,"StringTable sparse holes produced incorrect entry counts");
    require(contents(output/"text-candidates"/"localization"/"data"/"Text"/"ST_Character.jsonl")==text,"StringTable body was not published separately");
    memory.sceneResources(reflection);
    {
        ebon::resources::Dumper dumper(memory,reflection,config,output/"scene");
        require(dumper.run(),"scene and script objects were excluded from resources");
    }
    require(contents(output/"scene"/"resource_state.json").find("\"resources\":7")!=std::string::npos,"scene resource count was incorrect");
    require(contents(output/"scene"/"data"/"Maps"/"MapFixture.PersistentLevel.PlacedNPC.SceneRoot.jsonl").find("\"RelativeLocation\":{\"X\":1.25,\"Y\":-2.5,\"Z\":3.75}")!=std::string::npos,"reflected scene coordinates were missing");
    require(contents(output/"scene"/"data"/"Script"/"Engine.Default__PlacedNPC.jsonl").find("\"is_default\":true")!=std::string::npos,"scene CDO lost its default marker");
    require(contents(output/"scene"/"references.jsonl").find("RootComponent")!=std::string::npos,"scene object reference edges were missing");
    require(contents(output/"scene"/"scripts"/"resources.jsonl").find("Lua/LuaFixture.jsonl")!=std::string::npos,"Lua asset was not routed to scripts");
    require(contents(output/"scene"/"scripts"/"data"/"Lua"/"LuaFixture.jsonl")==contents(output/"scene"/"data"/"Lua"/"LuaFixture.jsonl"),"script body differs from original values");
    require(contents(output/"scene"/"maps"/"data"/"Maps"/"MapFixture.PersistentLevel.PlacedNPC.SceneRoot.jsonl")==contents(output/"scene"/"data"/"Maps"/"MapFixture.PersistentLevel.PlacedNPC.SceneRoot.jsonl"),"scene body was not published separately");
    require(!std::filesystem::exists(output/"scene"/"data"/"Engine"/"Transient.EvalFixture.jsonl"),"Evaluation class was incorrectly selected as Lua");
    require(contents(output/"scene"/"data"/"Engine"/"Transient.QuestRuntime.jsonl").find("\"CurrentQuestId\":5150")!=std::string::npos,"non-DataAsset gameplay manager was excluded");
    {
        auto focused=config;focused.values["resource_game_objects"]="0";
        ebon::resources::Dumper dumper(memory,reflection,focused,output/"scene-focused");
        require(dumper.run(),"disabling gameplay objects prevented scene capture");
    }
    require(!std::filesystem::exists(output/"scene-focused"/"data"/"Engine"/"Transient.QuestRuntime.jsonl"),"gameplay instance switch was ignored");
    {
        const auto reserved=output/"reserved-state";
        std::filesystem::create_directories(reserved);
        ebon::save(reserved/"resource_state.json","{\"status\":\"discovering\",\"success\":false}\n");
        struct HeldState {
            HANDLE value=INVALID_HANDLE_VALUE;
            ~HeldState(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
        } held;
        held.value=CreateFileW((reserved/"resource_state.json").c_str(),GENERIC_READ,
                              FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        require(held.value!=INVALID_HANDLE_VALUE,"cannot hold the state reader fixture");
        require(!DeleteFileW((reserved/"resource_state.json").c_str())&&GetLastError()==ERROR_SHARING_VIOLATION,
                "state reader fixture did not reproduce Windows delete sharing denial");
        ebon::resources::Dumper dumper(memory,reflection,config,reserved);
        require(dumper.run(true),"owned initializing state was incorrectly rejected");
        bool refused=false;
        try{
            ebon::resources::Dumper repeat(memory,reflection,config,reserved);
            repeat.run(true);
        }catch(const std::exception&){refused=true;}
        require(refused,"reserved state flag permitted overwriting completed resources");
    }
    require(!ebon::resources::scriptIdentity("/Script/Test.EvaluationEngine"),"Lua matcher accepted evaluation substring");
    require(ebon::resources::categories("SkinnedMesh") == std::vector<std::string>{"other_assets"},"rendering skin terms were misclassified");
    require(ebon::resources::categories("/GAME/MAPS/NPC_LUA") == ebon::resources::categories("/game/maps/npc_lua"),"resource categories depend on casing");
    {
        Fixture featureMemory;
        ebon::Reflection featureReflection(featureMemory,config,{Fixture::module,0x1000,L"fixture",L"fixture"});
        require(featureReflection.collect(),"feature fixture seed failed");
        featureReflection.moduleSha256="synthetic-feature-self-test";
        featureMemory.featureResources(featureReflection);
        ebon::resources::Dumper dumper(featureMemory,featureReflection,config,output/"features");
        require(dumper.run(),"target feature capture failed");
        featureMemory.networkFeatureResources(featureReflection,output/"features");
    }
    {
        Fixture priorityMemory;
        ebon::Reflection priorityReflection(priorityMemory,config,{Fixture::module,0x1000,L"fixture",L"fixture"});
        require(priorityReflection.collect(),"priority seed failed");
        priorityMemory.featureResources(priorityReflection);
        auto& tuning=priorityReflection.objects.back();
        tuning.path="/Game/Config/DA_Tuning.DA_Tuning";tuning.name="DA_Tuning";
        const auto tuningPath=tuning.path;
        priorityMemory.fname(tuning.address+24,tuning.name);
        priorityMemory.fname(0xa2e58,"/Game/Config/DA_Tuning");priorityMemory.fname(0xa2e60,"DA_Tuning");
        const auto hero=priorityReflection.objects[9];
        ebon::Object noise{311,0xa3400,hero.klass,"Default__BP_Noise_C",hero.className,
            "/Game/Characters/BP_Noise.Default__BP_Noise_C"};
        priorityMemory.object(noise.address,noise.klass,noise.index,noise.name);
        priorityMemory.put(noise.address+8,std::uint32_t(0x10));
        priorityReflection.objects.insert(priorityReflection.objects.begin(),noise);
        auto limited=config;limited.values["resource_feature_only"]="1";
        limited.values["resource_feature_max_objects"]="1";limited.values["resource_battle_max_objects"]="3";
        const auto folder=output/"priorities";bool called=false;
        ebon::resources::Dumper dumper(priorityMemory,priorityReflection,limited,folder);
        require(dumper.run(false,[&](){
            called=true;
            require(contents(folder/"feature_exports"/"battle_resources.jsonl").find(tuningPath)!=std::string::npos,
                    "unnamed battle dependency missing before network callback");
            require(contents(folder/"resource_state.json").find("collecting")!=std::string::npos,
                    "resource state completed before deferred network stage");
        }),"independent battle budget failed");
        require(called,"deferred stage was not called");
        const auto manifest=contents(folder/"resource_manifest.jsonl");
        require(manifest.find("DA_CharacterCombat")!=std::string::npos&&manifest.find(tuningPath)!=std::string::npos&&
                manifest.find(hero.path)!=std::string::npos&&manifest.find(noise.path)==std::string::npos,
                "menu/default noise starved a configuration or its dependencies");
        require(contents(folder/"feature_exports"/"priority_capture.json").find("\"object_limit_reached\":true")!=std::string::npos,
                "bounded priority capture hid its missing candidates");
        // Character roots have their own budget, and all roots precede even
        // the first root's dependency closure. Broad battle slots may be full.
        auto characterConfig=limited;characterConfig.values["resource_character_max_objects"]="2";
        characterConfig.values["resource_battle_max_objects"]="1";
        auto playerBase=priorityReflection.types[11];
        playerBase.address=0x86000;playerBase.path="/Script/HTGame.HTPlayerCharacter";
        priorityReflection.types.push_back(playerBase);
        for(auto& type:priorityReflection.types)if(type.address==hero.klass)type.super=playerBase.path;
        ebon::resources::Dumper characterDumper(priorityMemory,priorityReflection,characterConfig,output/"player-priorities");
        require(characterDumper.run(),"independent player-root capture failed");
        const auto playerManifest=contents(output/"player-priorities"/"resource_manifest.jsonl");
        require(playerManifest.find(hero.path)!=std::string::npos&&playerManifest.find(noise.path)!=std::string::npos,
                "player roots were starved by a dependency or the broad battle budget");
        require(contents(output/"player-priorities"/"feature_exports"/"priority_capture.json").find("\"characters\":{\"candidates\":2,\"visited\":2")!=std::string::npos,
                "player-root budget was not reported independently");
        auto focusedConfig=characterConfig;focusedConfig.values["resource_character_focus"]="/Game/Characters/BP_Hero.BP_Hero_C";
        ebon::resources::Dumper focusedDumper(priorityMemory,priorityReflection,focusedConfig,output/"player-focused");
        require(focusedDumper.run(),"focused player capture failed");
        const auto focusedManifest=contents(output/"player-focused"/"resource_manifest.jsonl");
        require(focusedManifest.find(hero.path)!=std::string::npos&&focusedManifest.find("DA_SystematicGameFeatures")==std::string::npos&&
                focusedManifest.find("DA_CharacterCombat")==std::string::npos,
                "focused character request still used unrelated broad menu/battle roots");
        const auto characterTableBefore=priorityReflection.objects[2];
        auto& characterTable=priorityReflection.objects[2];
        characterTable.name="DT_Character";characterTable.path="/Game/DataTable/Character/DT_Character.DT_Character";
        priorityMemory.fname(characterTable.address+24,characterTable.name);
        characterConfig.values["resource_character_max_objects"]="3";
        ebon::resources::Dumper tableDumper(priorityMemory,priorityReflection,characterConfig,output/"player-table-priorities");
        require(tableDumper.run(),"character table priority capture failed");
        const auto tableManifest=contents(output/"player-table-priorities"/"resource_manifest.jsonl");
        require(tableManifest.find(characterTable.path)!=std::string::npos&&tableManifest.find(hero.path)!=std::string::npos&&
                tableManifest.find(noise.path)!=std::string::npos,
                "character table and player roots did not share reserved priority slots");
        characterTable=characterTableBefore;priorityMemory.fname(characterTable.address+24,characterTable.name);
        // Restore the original fixture for the general-stage regressions.
        for(auto& type:priorityReflection.types)if(type.address==hero.klass)type.super.clear();
        priorityReflection.types.pop_back();
        ebon::resources::Dumper failing(priorityMemory,priorityReflection,limited,output/"deferred-failure");
        require(!failing.run(false,[](){throw std::runtime_error("synthetic network failure");}),"deferred failure was hidden");
        require(contents(output/"deferred-failure"/"resource_manifest.jsonl").find(tuningPath)!=std::string::npos&&
                contents(output/"deferred-failure"/"resource_state.json").find("synthetic network failure")!=std::string::npos,
                "deferred failure discarded completed resource evidence");
        // A menu asset can load the same generic dependency before the battle
        // stage. Reuse its body while retaining the complete battle closure.
        priorityMemory.fname(0x110048,"/Game/Characters/DA_CharacterCombat");
        priorityMemory.fname(0x110050,"DA_CharacterCombat");
        limited.values["resource_feature_max_objects"]="4096";
        ebon::resources::Dumper shared(priorityMemory,priorityReflection,limited,output/"shared-dependency");
        require(shared.run(),"shared menu/battle dependency failed");
        require(contents(output/"shared-dependency"/"feature_exports"/"battle_resources.jsonl").find("\"object\":"+ebon::quote(tuningPath))!=std::string::npos,
                "already exported generic dependency vanished from the battle closure");
    }
    {
        Fixture mapMemory;
        ebon::Reflection mapReflection(mapMemory,config,{Fixture::module,0x1000,L"fixture",L"fixture"});
        require(mapReflection.collect(),"map fixture seed failed");
        mapMemory.miniMapResources(mapReflection);
        auto focused=config;focused.values["resource_feature_only"]="1";
        const auto folder=output/"minimap-fog-blocks";
        ebon::resources::Dumper dumper(mapMemory,mapReflection,focused,folder);
        require(dumper.run(),"mini-map priority capture failed");
        const auto fogBody=contents(folder/"data"/"DataTable"/"DT_MiniMapFogBlock.jsonl");
        require(fogBody.find("\"row\":\"FogBlock_001\"")!=fogBody.npos&&
                fogBody.find("\"map_block_id\":\"FogBlock_001\"")!=fogBody.npos,
                "mini-map fog table row key was not exported as its map block ID");
        const auto mapRows=contents(folder/"feature_exports"/"feature_table_rows.jsonl");
        require(mapRows.find("\"map_block_id\":\"FogBlock_001\"")!=mapRows.npos,
                "mini-map fog row was missing from the feature table export");
        require(mapRows.find("\"table\":\"/Game/DataTable/MiniMap/DT_MiniMapFogRecord.DT_MiniMapFogRecord\"")!=mapRows.npos&&
                mapRows.find("\"row\":\"FogRecord_001\",\"values\":{\"FogID\":77}")!=mapRows.npos,
                "unreferenced MiniMapFogRecord table path or row values were not exported");
        require(contents(folder/"resource_manifest.jsonl").find("/Game/DataTable/MiniMap/DT_MiniMapFogRecord.DT_MiniMapFogRecord")!=std::string::npos,
                "direct MiniMap table root was not added to the resource manifest");
        require(contents(folder/"feature_exports"/"priority_capture.json").find("\"maps\":{\"candidates\":2")!=std::string::npos,
                "direct MiniMap table and DataAsset were not included in priority capture");
    }
    {
        Fixture mapBudgetMemory;
        ebon::Reflection mapBudgetReflection(mapBudgetMemory,config,{Fixture::module,0x1000,L"fixture",L"fixture"});
        require(mapBudgetReflection.collect(),"map budget fixture seed failed");
        mapBudgetMemory.featureResources(mapBudgetReflection);
        const auto menuTypes=mapBudgetReflection.types;const auto menuObjects=mapBudgetReflection.objects;
        mapBudgetMemory.miniMapResources(mapBudgetReflection);
        mapBudgetReflection.types.insert(mapBudgetReflection.types.end(),menuTypes.begin(),menuTypes.end());
        mapBudgetReflection.objects.insert(mapBudgetReflection.objects.end(),menuObjects.begin(),menuObjects.end());
        auto assetType=std::find_if(mapBudgetReflection.types.begin(),mapBudgetReflection.types.end(),[](const ebon::Type& type){
            return type.path=="/Script/HTGame.SystematicGameFeaturesDataAsset";
        });
        require(assetType!=mapBudgetReflection.types.end(),"menu budget fixture lost the feature asset schema");
        assetType->size=216;assetType->fields.push_back(mapBudgetMemory.property("OversizeText","StrProperty",200,16));
        for(unsigned i=0;i<8;++i){
            const auto address=0xa5000ull+std::uint64_t(i)*0x200;
            const auto name="DA_SystematicGameFeatures_"+std::to_string(i);
            const auto path="/Game/UI/"+name+"."+name;
            ebon::Object object{600+static_cast<int>(i),address,assetType->address,name,"SystematicGameFeaturesDataAsset",path};
            mapBudgetReflection.objects.push_back(object);mapBudgetMemory.object(address,assetType->address,object.index,name);
            mapBudgetMemory.fstring(address+200,std::wstring(60000,L'界'));
        }
        auto limited=config;limited.values["resource_feature_only"]="1";limited.values["resource_max_output_mb"]="1";
        const auto folder=output/"minimap-byte-budget";
        auto control=limited;control.values["resource_max_output_mb"]="32";
        ebon::resources::Dumper controlDumper(mapBudgetMemory,mapBudgetReflection,control,output/"minimap-byte-budget-control");
        require(controlDumper.run(),"unbounded map budget control capture failed");
        require(contents(output/"minimap-byte-budget-control"/"resource_manifest.jsonl").find("/Game/DataTable/MiniMap/DT_MiniMapFogRecord.DT_MiniMapFogRecord")!=std::string::npos,
                "map budget fixture did not discover its direct table root");
        ebon::resources::Dumper dumper(mapBudgetMemory,mapBudgetReflection,limited,folder);
        require(!dumper.run(),"oversized menu fixture unexpectedly fit its byte budget");
        require(contents(folder/"resource_manifest.jsonl").find("/Game/DataTable/MiniMap/DT_MiniMapFogRecord.DT_MiniMapFogRecord")!=std::string::npos&&
                contents(folder/"data"/"DataTable"/"MiniMap"/"DT_MiniMapFogRecord.jsonl").find("FogRecord_001")!=std::string::npos,
                "menu byte-budget exhaustion prevented the earlier MiniMap table export");
        require(contents(folder/"resource_state.json").find("byte budget exceeded")!=std::string::npos,
                "MiniMap priority budget regression did not exercise byte exhaustion");
    }
    {
        Fixture longMemory;
        ebon::Reflection longReflection(longMemory,config,{Fixture::module,0x1000,L"fixture",L"fixture"});
        require(longReflection.collect(),"long path seed failed");
        longMemory.sceneResources(longReflection);
        longReflection.objects[7].path="/Engine/Transient."+std::string(290,'W')+".QuestRuntime";
        ebon::resources::Dumper dumper(longMemory,longReflection,config,output/"long-path");
        require(dumper.run(),"long widget Outer path prevented capture");
        require(contents(output/"long-path"/"resource_manifest.jsonl").find("data/_long/object_")!=std::string::npos,"long path did not use a short filename");
    }
    {
        Fixture rpcMemory;constexpr std::uint64_t params=0x1a0000,wrappers=0x1a1000,payload=0x1a2000;
        rpcMemory.put(params,std::int32_t(301));
        rpcMemory.put(params+8,ebon::rpc_samples::ArrayHeader{wrappers,1,1});
        rpcMemory.put(params+24,std::int32_t(0));
        rpcMemory.put(wrappers,std::uint8_t(14));rpcMemory.put(wrappers+4,std::uint32_t(77));rpcMemory.put(wrappers+8,std::uint32_t(2));
        rpcMemory.put(wrappers+16,ebon::rpc_samples::ArrayHeader{payload,3,3});
        const std::uint8_t raw[]={0xaa,0xbb,0xcc};rpcMemory.putBytes(payload,raw,sizeof(raw));
        const ebon::rpc_samples::RpcLayout schema{32,0,8,0,16,32,0,0,4,16};
        const std::map<std::int64_t,std::string> kinds={{14,"String"}};std::string sample;
        ebon::Property wrapperArray;wrapperArray.type="ArrayProperty";wrapperArray.element=16;
        wrapperArray.detail="{\"inner\":{\"type\":\"StructProperty\",\"size\":32,\"target\":\"/Script/HottaFramework.ParameterWrapper\"}}";
        require(ebon::rpc_samples::wrapperArrayLayoutMatches(&wrapperArray,32),
                "TArray wrapper element layout was rejected despite a verified inner struct size");
        require(!ebon::rpc_samples::wrapperArrayLayoutMatches(&wrapperArray,24),
                "mismatched TArray wrapper element size was accepted");
        require(ebon::rpc_samples::captureClientGameDataRpc(rpcMemory,params,schema,301,8,kinds,0x123456,0x654321,42,1234,sample),
                "valid ClientGameDataRPC call sample was rejected");
        require(sample.find("\"message_id\":301")!=sample.npos&&sample.find("\"type_name\":\"String\"")!=sample.npos&&
                sample.find("\"captured_payload_hex\":\"aabbcc\"")!=sample.npos&&sample.find("\"sample_id\":42")!=sample.npos&&
                sample.find("\"tick_ms\":1234")!=sample.npos&&sample.find("\"params_truncated\":false")!=sample.npos&&
                sample.find("\"wire_packet_bytes_captured\":false")!=sample.npos,
                "RPC sample lost its message ID, reflected wrapper type, payload, or wire limitation");
        require(!ebon::rpc_samples::captureClientGameDataRpc(rpcMemory,params,schema,300,8,kinds,0,0,43,1235,sample),
                "explicit message ID filter accepted an unrelated sample");
        require(ebon::rpc_samples::captureClientGameDataRpc(rpcMemory,params,schema,-1,2,kinds,0,0,44,1236,sample)&&
                sample.find("\"captured_payload_hex\":\"aabb\"")!=sample.npos&&sample.find("\"truncated\":true")!=sample.npos,
                "all-ID sampling did not mark a bounded payload as truncated");
        auto invalid=schema;invalid.parameters=30;
        require(!ebon::rpc_samples::captureClientGameDataRpc(rpcMemory,params,invalid,-1,8,kinds,0,0,47,1239,sample),
                "out-of-bounds ClientGameDataRPC parameter layout was accepted");
    }
    {
        Fixture fogMemory;constexpr std::uint64_t params=0x1b0000;
        fogMemory.fstring(params,L"FogRecord_001");fogMemory.put(params+16,std::int32_t(2));fogMemory.put(params+20,std::int32_t(7));
        const std::uint8_t oldRow[]={0x10,0x20,0x30,0x40,0x50,0x60,0x70,0x80};fogMemory.putBytes(params+24,oldRow,sizeof(oldRow));
        const ebon::rpc_samples::FogLayout schema{32,0,16,20,24,8};std::string sample;
        require(ebon::rpc_samples::captureFogRecord(fogMemory,params,schema,0x123456,0x654321,45,1237,sample),
                "valid MiniMap fog-record callback sample was rejected");
        require(sample.find("\"record_name\":\"FogRecord_001\"")!=sample.npos&&sample.find("\"change_type\":2")!=sample.npos&&
                sample.find("\"row_index\":7")!=sample.npos&&sample.find("\"old_row_hex\":\"1020304050607080\"")!=sample.npos&&
                sample.find("\"sample_id\":45")!=sample.npos&&sample.find("\"wire_packet_bytes_captured\":false")!=sample.npos,
                "fog callback sample lost its record name, change metadata, old-row bytes, or capture limitation");
        auto invalid=schema;invalid.oldRowSize=16;
        require(!ebon::rpc_samples::captureFogRecord(fogMemory,params,invalid,0,0,46,1238,sample),
                "out-of-bounds fog row layout was accepted");
    }
    testRpcSamplerBinding(output);
    {
        Fixture names;constexpr std::uint64_t at=0x1c0000;
        names.fname(at,"MiniMapFogRecord");ebon::rpc_samples::SyncName name;
        require(names.get(at,name),"sync property-name fixture is unreadable");const ebon::Layout layout(config);
        require(ebon::rpc_samples::syncResolveName(names,Fixture::pool,&layout,name)=="MiniMapFogRecord",
                "replicated-property FName did not resolve through the verified name pool");
        name.number=3;
        require(ebon::rpc_samples::syncResolveName(names,Fixture::pool,&layout,name)=="MiniMapFogRecord_2",
                "replicated-property FName instance number was lost");
    }
    std::cout<<"Resource self-test passed: resource values, budgets, RPC override installation, argument forwarding and sampling status\n";
    return 0;
}
} // namespace

int wmain(int argc,wchar_t** argv) {
    try {
        const auto output=argc>1?std::filesystem::path(argv[1]):
            std::filesystem::temp_directory_path()/("ebon-resource-self-test-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
        require(!std::filesystem::exists(output)||std::filesystem::is_empty(output),"self-test output must be new or empty");
        return run(output);
    }catch(const std::exception& e){std::cerr<<"Resource self-test failed: "<<e.what()<<'\n';return 1;}
}
