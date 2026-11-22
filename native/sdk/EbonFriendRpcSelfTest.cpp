// Observes synthetic Friend RPC invocations in this process only.
#include "../dumper/ebon_rpc_sample_capture.hpp"

namespace {
using Event=void(__fastcall*)(void*,void*,void*);
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
std::string contents(const std::filesystem::path& path){
    std::ifstream file(path,std::ios::binary);require(bool(file),"missing sample output");
    return std::string((std::istreambuf_iterator<char>(file)),{});
}
volatile LONG originalCalls=0,eventCalls=0;
void __fastcall native(void*,void*,void* result){InterlockedIncrement(&originalCalls);if(result)*static_cast<int*>(result)=77;}
void invokeNative(void* object,std::uint64_t function,void* params,void* result=nullptr,std::uint64_t code=0){
    alignas(8) std::array<std::uint64_t,20> frame{};
    frame[2]=function;frame[3]=reinterpret_cast<std::uint64_t>(object);frame[4]=code;
    frame[5]=reinterpret_cast<std::uint64_t>(params);frame[18]=function;
    reinterpret_cast<Event>(*reinterpret_cast<std::uint64_t*>(function+216))(object,frame.data(),result);
}
void __fastcall processEvent(void* object,void* function,void* params){
    InterlockedIncrement(&eventCalls);invokeNative(object,reinterpret_cast<std::uint64_t>(function),params);
}
ebon::Property field(const char* name,const char* type,int offset,int size,const char* target=""){
    ebon::Property p;p.name=name;p.type=type;p.offset=offset;p.element=size;p.array=1;p.target=target;return p;
}
ebon::Type type(const char* kind,const char* path,int size,std::uint64_t address=0){
    ebon::Type t;t.kind=kind;t.path=path;t.size=size;t.address=address;return t;
}
}

int wmain(int argc,wchar_t** argv){
    try{
        require(argc==2,"supply output directory");const std::filesystem::path out=argv[1];
        ebon::InProcessReader memory;const auto module=ebon::findModule(GetCurrentProcessId(),L"");
        const auto eventRva=reinterpret_cast<std::uint64_t>(&processEvent)-module.base;
        const auto nativeRva=reinterpret_cast<std::uint64_t>(&native)-module.base;
        ebon::Config config;config.values={{"game_rpc_sample_capture","1"},{"exe_sha256","fixture"},
            {"game_rpc_sample_friend_only","1"},
            {"process_event_vtable_index","76"},{"process_event_rva",ebon::hex(eventRva)},
            {"game_rpc_native_capture","1"},{"function_native_offset","216"},
            {"game_rpc_native_rpc_rva",ebon::hex(nativeRva)},{"game_rpc_native_friend_client_rva",ebon::hex(nativeRva)},
            {"game_rpc_native_friend_server_rva",ebon::hex(nativeRva)},
            {"game_rpc_frame_node_offset","16"},{"game_rpc_frame_object_offset","24"},
            {"game_rpc_frame_code_offset","32"},{"game_rpc_frame_locals_offset","40"},
            {"game_rpc_frame_current_function_offset","144"},{"game_rpc_sample_auto_arm","1"},
            {"game_rpc_sample_limit","4"},{"game_rpc_sample_candidate_msg_id","999"}};
        config.values["game_rpc_sample_window_seconds"]="1";
        ebon::Reflection reflection(memory,config,module);reflection.moduleSha256="fixture";
        std::string rootTable="{\"object\":\"/Script/CoreUObject.Default__Object\",\"entries\":[";
        for(unsigned i=0;i<=76;++i){if(i)rootTable+=',';rootTable+=i==76?ebon::quote(ebon::hex(eventRva)):"null";}
        reflection.runtimeVtables={rootTable+"]}"};
        alignas(8) static std::array<std::uint64_t,32> gameFunction{},clientFunction{},serverFunction{};
        for(auto p:{gameFunction.data(),clientFunction.data(),serverFunction.data()})p[27]=reinterpret_cast<std::uint64_t>(&native);
        const auto gameAddress=reinterpret_cast<std::uint64_t>(gameFunction.data());
        const auto clientAddress=reinterpret_cast<std::uint64_t>(clientFunction.data()),serverAddress=reinterpret_cast<std::uint64_t>(serverFunction.data());
        auto owner=type("class","/Script/HTGame.HTPlayerState",64,0x50000);
        auto game=type("function","/Script/HTGame.HTPlayerState.ClientGameDataRPC",32,gameAddress);
        game.parameterSize=std::uint16_t(32);game.nativeRva=nativeRva;
        game.fields={field("InMsgID","IntProperty",0,4),field("InParameters","StructProperty",8,24,"/Script/HottaFramework.ParameterWrapperArray")};
        auto params=type("struct","/Script/HottaFramework.ParameterWrapperArray",24,0x61000);
        auto array=field("ParameterWrappers","ArrayProperty",0,16);
        array.detail="{\"inner\":{\"type\":\"StructProperty\",\"size\":32,\"target\":\"/Script/HottaFramework.ParameterWrapper\"}}";
        params.fields={array,field("ReadParameterIndex","IntProperty",16,4)};
        auto wrapper=type("struct","/Script/HottaFramework.ParameterWrapper",32,0x62000);
        auto binary=field("BinaryData","ArrayProperty",16,16);binary.detail="{\"inner\":{\"type\":\"ByteProperty\"}}";
        wrapper.fields={field("TypeInfo","StructProperty",0,12,"/Script/HottaFramework.ParameterTypeInfo"),binary};
        auto info=type("struct","/Script/HottaFramework.ParameterTypeInfo",12,0x63000);
        info.fields={field("Type","EnumProperty",0,1),field("CustomTypeName","NameProperty",4,8)};
        auto message=type("struct","/Script/HottaFramework.HottaRpcMessage",32,0x64000);message.super=params.path;
        message.fields={field("Type","IntProperty",24,4)};
        auto client=type("function","/Script/HTGame.HTPlayerState.Client_Friend_RPC",32,clientAddress);
        client.parameterSize=std::uint16_t(32);client.nativeRva=nativeRva;
        client.fields={field("InMessage","StructProperty",0,32,message.path.c_str())};
        auto server=client;server.path="/Script/HTGame.HTPlayerState.Server_Friend_RPC";server.address=serverAddress;
        ebon::rpc_samples::RpcLayout layout;
        require(ebon::rpc_samples::friendRpcLayout(&client,&message,&params,&wrapper,&info,layout)&&
            layout.messageId==24&&layout.parameters==0&&layout.readIndex==16,"friend inheritance layout not accepted");
        auto badMessage=message;badMessage.fields[0].offset=0;
        require(!ebon::rpc_samples::friendRpcLayout(&client,&badMessage,&params,&wrapper,&info,layout),"wrong Type offset accepted");
        require(!ebon::rpc_samples::friendRpcLayout(&game,&message,&params,&wrapper,&info,layout),"GameDataRPC accepted as Friend_RPC");
        reflection.types={owner,game,params,wrapper,info,message,client,server};
        alignas(8) static std::array<std::uint64_t,96> table{};table[76]=reinterpret_cast<std::uint64_t>(&processEvent);
        alignas(8) static std::uint64_t object=reinterpret_cast<std::uint64_t>(table.data());
        ebon::Object cdo;cdo.address=reinterpret_cast<std::uint64_t>(&object);cdo.klass=owner.address;
        cdo.name="Default__HTPlayerState";cdo.path="/Script/HTGame.Default__HTPlayerState";reflection.objects={cdo};
        std::string status;
        {
            auto bad=config;bad.values["exe_sha256"]="wrong";ebon::rpc_samples::Capture rejected;
            require(!rejected.install(memory,reflection,bad,module,out/"rejected",status)&&status=="profile_hash_mismatch"&&
                table[76]==reinterpret_cast<std::uint64_t>(&processEvent)&&clientFunction[27]==reinterpret_cast<std::uint64_t>(&native),
                "hash mismatch modified a target");
        }
        static ebon::rpc_samples::Capture capture;
        require(capture.install(memory,reflection,config,module,out/"capture",status),"sampler installation failed");
        const auto state=contents(out/"capture"/"game_rpc_sample_state.json");
        require(state.find("\"friend_client_native_installed\":true")!=state.npos&&
            state.find("\"friend_server_native_installed\":true")!=state.npos,"friend native functions not installed");
        require(state.find("\"capture_scope\":\"friends\"")!=state.npos&&
            state.find("\"friend_diagnosis\":\"friend_functions_not_observed\"")!=state.npos&&
            gameFunction[27]==reinterpret_cast<std::uint64_t>(&native),"focused capture installed unrelated native hooks or claimed samples");
        ebon::save(out/"capture"/"empty_friend_state.json",state);
        alignas(8) std::array<std::uint8_t,32> data{},wrappers{};
        const std::array<std::uint8_t,3> payload{1,2,3};
        const ebon::rpc_samples::ArrayHeader wrapperHeader{reinterpret_cast<std::uint64_t>(wrappers.data()),1,1};
        const ebon::rpc_samples::ArrayHeader payloadHeader{reinterpret_cast<std::uint64_t>(payload.data()),3,3};
        std::memcpy(data.data(),&wrapperHeader,16);const std::int32_t messageType=42;
        std::memcpy(data.data()+24,&messageType,4);wrappers[0]=6;std::memcpy(wrappers.data()+16,&payloadHeader,16);
        // Unrelated login/sync calls must not consume the Friend sample budget.
        constexpr LONG noiseCalls=8;
        for(LONG i=0;i<noiseCalls;++i)reinterpret_cast<Event>(table[76])(&object,reinterpret_cast<void*>(gameAddress),data.data());
        reinterpret_cast<Event>(table[76])(&object,reinterpret_cast<void*>(clientAddress),data.data());
        reinterpret_cast<Event>(table[76])(&object,reinterpret_cast<void*>(serverAddress),data.data());
        int result=0;invokeNative(&object,clientAddress,data.data(),&result);require(result==77,"native result lost");
        invokeNative(&object,serverAddress,data.data());capture.flush();
        ebon::save(out/"capture"/"captured_friend_state.json",contents(out/"capture"/"game_rpc_sample_state.json"));
        auto samples=contents(out/"capture"/"game_rpc_samples.jsonl");
        require(std::count(samples.begin(),samples.end(),'\n')==4,"friend RPC dual entry was duplicated or omitted");
        require(samples.find("\"message_type\":42")!=samples.npos&&samples.find("\"captured_payload_hex\":\"010203\"")!=samples.npos&&
            samples.find("\"kind\":\"ClientGameDataRPC\"")==samples.npos,"friend payload used GameDataRPC layout");
        // The four-sample cap drops another valid call, and bad native frames
        // remain forwarded while producing no sample.
        invokeNative(&object,serverAddress,data.data());invokeNative(&object,clientAddress,data.data(),nullptr,1);capture.flush();
        require(contents(out/"capture"/"game_rpc_samples.jsonl")==samples&&originalCalls==6+noiseCalls&&eventCalls==2+noiseCalls,
            "sample limit or original forwarding failed");
        ebon::save(out/"capture"/"rpc_sample_capture.request","synthetic request\n");capture.pollRequest();
        const auto rearmed=contents(out/"capture"/"game_rpc_sample_state.json");
        require(!std::filesystem::exists(out/"capture"/"rpc_sample_capture.request")&&
            rearmed.find("\"window_id\":2")!=rearmed.npos&&rearmed.find("\"arm_source\":\"request_file\"")!=rearmed.npos,
            "request-file rearm was not acknowledged");
        ebon::save(out/"capture"/"rearmed_friend_state.json",rearmed);
        const std::int32_t badCount=-1;std::memcpy(data.data()+8,&badCount,4);
        invokeNative(&object,clientAddress,data.data());capture.flush();
        require(contents(out/"capture"/"game_rpc_samples.jsonl")==samples&&originalCalls==7+noiseCalls,"bad wrapper count was captured or swallowed");
        // Resolve a custom FName through a bounded synthetic name pool while preserving its raw ID.
        std::memcpy(data.data(),&wrapperHeader,16);wrappers[0]=38;
        const std::uint32_t nameIndex=16;std::memcpy(wrappers.data()+4,&nameIndex,4);
        alignas(8) std::array<std::uint8_t,32> pool{};std::array<std::uint8_t,128> block{};
        const std::uint64_t blockAddress=reinterpret_cast<std::uint64_t>(block.data());std::memcpy(pool.data()+16,&blockAddress,8);
        const std::string typeName="HTFriendInfo";const std::uint16_t header=std::uint16_t(typeName.size()<<6);
        std::memcpy(block.data()+32,&header,2);std::memcpy(block.data()+34,typeName.data(),typeName.size());
        const std::uint32_t cursor=std::uint32_t(34+typeName.size());std::memcpy(pool.data()+12,&cursor,4);
        ebon::Layout names(config);names.poolBlock=8;names.poolCursor=12;names.poolBlocks=16;names.nameBits=16;
        names.nameStride=2;names.nameHeader=2;names.nameShift=6;names.nameWide=1;names.maxNameLength=1023;
        require(ebon::rpc_samples::friendRpcLayout(&client,&message,&params,&wrapper,&info,layout),"friend schema changed");
        std::string custom;
        require(ebon::rpc_samples::captureClientGameDataRpc(memory,reinterpret_cast<std::uint64_t>(data.data()),layout,-1,2,{},
            0,clientAddress,99,1,custom,"synthetic","Client_Friend_RPC",client.path.c_str(),"client","fixture",
            reinterpret_cast<std::uint64_t>(pool.data()),&names)&&custom.find("\"custom_type_name\":\"HTFriendInfo\"")!=custom.npos,
            "custom wrapper name was not resolved");
        ebon::save(out/"capture"/"custom_friend_sample.json",custom);
        Sleep(1100);capture.flush();
        const auto expired=contents(out/"capture"/"game_rpc_sample_state.json");
        Sleep(1100);capture.flush();
        require(expired!=contents(out/"capture"/"game_rpc_sample_state.json")&&
            expired.find("\"status\":\"window_expired\"")!=expired.npos,"expired window stopped publishing its heartbeat");
        std::cout<<"Friend RPC synthetic sampling passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
