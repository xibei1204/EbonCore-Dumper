#include "ebon_reflection.hpp"
#include "ebon_network_version_selftest.hpp"
#include <cassert>

class Snapshot final:public ebon::MemoryReader {
    static constexpr std::uint64_t base=0x10000;
    std::vector<std::uint8_t> bytes=std::vector<std::uint8_t>(0x900000);
public:
    mutable std::size_t copyCalls=0,regionCalls=0,invalidReads=0;
    std::vector<std::pair<std::uint64_t,std::uint64_t>> deniedRanges;
    std::uint64_t regionSpan=0x1000;
    template<class T>void put(std::uint64_t a,T value){assert(a>=base&&a+sizeof(T)<=base+bytes.size());std::memcpy(bytes.data()+a-base,&value,sizeof(T));}
    void putBytes(std::uint64_t a,const void* p,std::size_t n){assert(a>=base&&a+n<=base+bytes.size());std::memcpy(bytes.data()+a-base,p,n);}
    bool copy(std::uint64_t a,void* p,std::size_t n)const override {++copyCalls;if(!p||a<base||a+n<a||a+n>base+bytes.size()){++invalidReads;return false;}for(const auto& range:deniedRanges)if(a<range.second&&a+n>range.first){++invalidReads;return false;}std::memcpy(p,bytes.data()+a-base,n);return true;}
    std::vector<ebon::Region> regions(const ebon::Module&)const override{++regionCalls;return {{0x10000,regionSpan,false,true}};}
    bool executable(std::uint64_t p)const override{return p>=0x10800&&p<0x11000;}
    static constexpr std::uint64_t module=0x10000,pool=0x10200,array=0x10300,block=0x30000,chunks=0x40000,items=0x41000,fieldClass=0x50000,field=0x51000;
    static constexpr std::uint64_t objClass=0x60000,objStruct=0x60100,objFunction=0x60200,objEnum=0x60300,objExample=0x60400,objActor=0x60500;
    int run(const std::filesystem::path& output) {
        bool f10Down=false;
        const auto f10DownState=std::numeric_limits<SHORT>::min();
        if(ebon::keyPressedEdge(0,f10Down)||!ebon::keyPressedEdge(1,f10Down)||
           ebon::keyPressedEdge(0,f10Down)||!ebon::keyPressedEdge(f10DownState,f10Down)||
           ebon::keyPressedEdge(f10DownState,f10Down)||ebon::keyPressedEdge(0,f10Down)||
           !ebon::isForegroundForGame(7,7,8)||!ebon::isForegroundForGame(8,7,8)||
           ebon::isForegroundForGame(9,7,8)||ebon::isForegroundForGame(8,7,0))return 31;
        ebon::Config c;c.values={{"module","HTGame.exe"},{"name_pool_rva","0x200"},{"object_array_rva","0x300"}};
        std::vector<std::string> strings={"None","ByteProperty","IntProperty","BoolProperty","ScriptStruct","Class","Function","Enum","EExample","FExample","Actor","Health","Visible","Extra","UserDefinedStruct"};std::map<std::string,std::uint32_t> ids;std::uint32_t cursor=0;
        for(std::size_t i=0;i<strings.size();++i){const auto& s=strings[i];ids[s]=cursor/2;auto off=block+cursor;std::uint16_t h=(std::uint16_t)(s.size()<<6);put(off,h);putBytes(off+2,s.data(),s.size());cursor+=(std::uint32_t)((2+s.size()+1)&~1u);}
        put(pool+16,block);put(pool+12,cursor);put(pool+8,std::uint32_t(0));put(array+16,chunks);put(chunks,items);put(array+36,std::int32_t(6));put(array+32,std::int32_t(65536));put(array+44,std::int32_t(1));put(array+40,std::int32_t(1));
        struct O{std::uint64_t p,klass;const char* name;};O objs[]={{objClass,objClass,"Class"},{objStruct,objClass,"ScriptStruct"},{objFunction,objClass,"Function"},{objEnum,objClass,"Enum"},{objExample,objEnum,"EExample"},{objActor,objStruct,"Actor"}};
        std::uint64_t vtable=0x10800,fn=0x10840;put(vtable,fn);
        for(int i=0;i<6;++i){auto p=objs[i].p;put(items+(std::uint64_t)i*24,p);put(p,std::uint64_t(0x10800));put(p+12,std::int32_t(i));put(p+16,objs[i].klass);put(p+24,ids[objs[i].name]);put(p+28,std::uint32_t(0));put(p+32,std::uint64_t(0));put(p+64,std::uint64_t(0));put(p+80,std::uint64_t(0));put(p+88,std::int32_t(16));put(p+176,std::uint32_t(0));put(p+182,std::uint16_t(0));put(p+216,std::uint64_t(0));put(p+64+8,std::uint64_t(0));}
        put(objActor+80,field);put(objActor+88,std::int32_t(16));
        put(fieldClass,ids["IntProperty"]);put(fieldClass+4,std::uint32_t(0));put(field+0,std::uint64_t(0x10800));put(field+8,fieldClass);put(field+32,std::uint64_t(0));put(field+40,ids["Health"]);put(field+44,std::uint32_t(0));put(field+56,std::int32_t(1));put(field+60,std::int32_t(4));put(field+64,std::uint64_t(0));put(field+76,std::int32_t(0));
        auto boolClass=fieldClass+0x100,nextField=field+0x100;put(boolClass,ids["BoolProperty"]);put(nextField+8,boolClass);put(nextField+32,std::uint64_t(0));put(nextField+40,ids["Visible"]);put(nextField+44,std::uint32_t(0));put(nextField+56,std::int32_t(1));put(nextField+60,std::int32_t(1));put(nextField+64,std::uint64_t(0));put(nextField+76,std::int32_t(4));put(field+32,nextField);
        put(objExample+64,std::uint64_t(0));put(objExample+72,std::int32_t(0));put(objExample+76,std::int32_t(0));
        ebon::Module mod{module,0x1000,L"HTGame.exe",L"synthetic"};ebon::Reflection r(*this,c,mod);bool ok=r.collect();r.write(output,ok);
        if(!ok||r.types.empty()||!r.propertyCount){for(const auto& e:r.errors)std::cerr<<e<<"\n";return 1;}
        std::ifstream f(output/"types.jsonl",std::ios::binary);std::string text((std::istreambuf_iterator<char>(f)),{});
        if(text.find("\"name\":\"Health\"")==std::string::npos||text.find("\"type\":\"IntProperty\"")==std::string::npos||text.find("\"name\":\"Visible\"")==std::string::npos||text.find("\"size\":4")==std::string::npos)return 2;
        // MinAlignment occupies the low 16 bits; the upper word belongs to
        // other metadata and must not discard a valid 8-byte alignment.
        put(objActor+92,std::uint32_t(0x10008));
        ebon::Reflection aligned(*this,c,mod);
        if(!aligned.collect())return 40;
        const auto alignedActor=std::find_if(aligned.types.begin(),aligned.types.end(),[](const auto& t){return t.name=="Actor";});
        if(alignedActor==aligned.types.end()||alignedActor->alignment!=8u||alignedActor->headerEvidence.empty())return 41;
        put(objActor+16,objClass);put(objActor+88,std::int32_t(14));
        ebon::Reflection classSpan(*this,c,mod);
        if(!classSpan.collect())return 45;
        const auto spanActor=std::find_if(classSpan.types.begin(),classSpan.types.end(),[](const auto& t){return t.name=="Actor";});
        if(spanActor==classSpan.types.end()||spanActor->alignment!=8u||spanActor->size!=14)return 46;
        put(objActor+16,objStruct);put(objActor+88,std::int32_t(16));
        put(objActor+92,std::uint32_t(0));
        put(objStruct+24,ids["UserDefinedStruct"]);put(field+120,objActor);
        ebon::Reflection userDefined(*this,c,mod);
        if(!userDefined.collect())return 43;
        std::string userTarget;std::optional<unsigned> userMask;
        userDefined.propertyDetail(field,"StructProperty",16,userTarget,userMask,0);
        if(userTarget!="Actor")return 44;
        put(objStruct+24,ids["ScriptStruct"]);put(field+120,std::uint64_t(0));
        std::ifstream s(output/"dump_state.json",std::ios::binary);std::string state((std::istreambuf_iterator<char>(s)),{});
        if(state.find("\"layout_verified\":true")==std::string::npos||state.find("\"property_layout_verified\":true")==std::string::npos||state.find("\"status\":\"complete_with_unresolved\"")==std::string::npos||state.find("\"success\":true")==std::string::npos||state.find("BoolProperty mask unavailable") == std::string::npos)return 3;
        auto automatic=output/"automatic";ebon::Config autoConfig;autoConfig.values={{"module","HTGame.exe"},{"name_pool_rva","0"},{"object_array_rva","0"}};ebon::Reflection autoReflection(*this,autoConfig,mod);bool autoOk=autoReflection.collect();autoReflection.write(automatic,autoOk);if(!autoOk||autoReflection.poolAddress()!=pool||autoReflection.objectArrayAddress()!=array)return 4;
        // Network fields include valid zero indices. Verify ClassReps by actual
        // property pointers/index/array slots and keep wire IDs unavailable.
        auto networkConfig=c;
        networkConfig.values["function_rpc_id_offset"]="0xba";
        networkConfig.values["function_rpc_response_id_offset"]="0xbc";
        networkConfig.values["property_rep_index_offset"]="0x48";
        networkConfig.values["property_rep_condition_offset"]="0x4a";
        networkConfig.values["property_rep_notify_offset"]="0x70";
        put(objActor+16,objClass);put(field+64,std::uint64_t(0x20));
        put(field+72,std::uint16_t(0));put(field+74,std::uint8_t(3));put(field+112,ids["Extra"]);
        put(objExample+16,objFunction);put(objExample+32,objActor);put(objExample+176,std::uint32_t(0x2001c0));
        put(objExample+186,std::uint16_t(17));put(objExample+188,std::uint16_t(31));
        const std::uint64_t repRecords=0x54000,rpcRecords=0x55000;
        put(repRecords,field);put(repRecords+8,std::int32_t(0));put(rpcRecords,objExample);
        put(objActor+0x100,repRecords);put(objActor+0x108,std::int32_t(1));put(objActor+0x10c,std::int32_t(1));
        put(objActor+0x120,rpcRecords);put(objActor+0x128,std::int32_t(1));put(objActor+0x12c,std::int32_t(1));
        ebon::Reflection network(*this,networkConfig,mod);
        if(!network.collect()||network.rpcCount!=1||network.replicatedPropertyCount!=1||network.classNetworkTables.size()!=2)return 22;
        const auto networkJson=network.networkJson();
        if(networkJson.find("\"rpc_id\":17")==networkJson.npos||networkJson.find("\"rpc_response_id\":31")==networkJson.npos||
           networkJson.find("\"rep_index\":0")==networkJson.npos||networkJson.find("\"rep_notify\":\"Extra\"")==networkJson.npos||
           networkJson.find("\"field_net_index\":null")==networkJson.npos||networkJson.find("\"rep_layout_handle\":null")==networkJson.npos)return 23;
        const std::function<void()> noDeadline=[](){};
        std::map<std::uint64_t,ebon::NetworkRuntimeClass> runtimeClasses={
            {objActor,{objActor,5,"Actor","Base"}},{objClass,{objClass,0,"Base",""}}};
        std::map<std::uint64_t,ebon::NetworkRuntimeField> runtimeFields={
            {objExample,{objExample,"Actor.Extra","Actor",0,true,1}},
            {field,{field,"Actor.Health","Actor",0,false,4}}};
        ebon::NetworkRuntimeDecoder runtime(*this,runtimeClasses,runtimeFields,noDeadline);
        put(0x57000,std::uint64_t(0x58000));put(0x58000,std::int32_t(10));put(0x58008,std::uint64_t(0x59000));
        put(0x58010,std::int32_t(5));put(0x58014,std::int32_t(3));put(0x58020,std::uint64_t(0x5a000));
        put(0x58028,std::int32_t(1));put(0x5802c,std::int32_t(1));
        put(0x59010,std::int32_t(0));put(0x59014,std::int32_t(2));put(0x59028,std::int32_t(10));
        put(0x5a000,objExample);put(0x5a008,std::uint32_t(10));put(0x5a00c,std::uint32_t(123));
        const auto decodedCache=runtime.decodeCache(0x58000);
        if(decodedCache.empty()||decodedCache.find("\"field_net_index\":10")==std::string::npos)return 24;
        put(0x5a008,std::uint32_t(9));if(!runtime.decodeCache(0x58000).empty())return 25;
        put(0x5a008,std::uint32_t(10));
        put(0x5b060,std::uint64_t(0x5c000));put(0x5b068,std::int32_t(1));put(0x5b06c,std::int32_t(1));
        put(0x5b070,std::uint64_t(0x5d000));put(0x5b078,std::int32_t(2));put(0x5b07c,std::int32_t(2));
        put(0x5b0a0,objActor);
        put(0x5c000,field);put(0x5c018,std::uint16_t(0));put(0x5c01a,std::uint16_t(1));
        put(0x5d000,field);put(0x5d014,std::uint16_t(1));put(0x5d018,std::uint32_t(456));put(0x5d01c,std::uint8_t(5));
        put(0x5d03c,std::uint8_t(1));
        const auto decodedLayout=runtime.decodeLayout(0x5b000);
        if(decodedLayout.empty()||decodedLayout.find("\"relative_handle\":1")==std::string::npos)return 26;
        put(0x5d016,std::uint16_t(8));if(!runtime.decodeLayout(0x5b000).empty())return 27;
        put(0x5d016,std::uint16_t(0));put(0x63000,std::uint64_t(0x5b000));
        const auto focusedScan=runtime.scan({0x63000},mod,1000,{objActor,objClass},{ {0x63000,768} });
        if(focusedScan.roots!=1||focusedScan.nodes<2||focusedScan.layouts.size()!=1||
           focusedScan.layouts[0].find("\"class\":\"Actor\"")==std::string::npos){
            std::cerr<<"focused scan roots="<<focusedScan.roots<<" nodes="<<focusedScan.nodes<<" layouts="<<focusedScan.layouts.size()<<" exhausted="<<focusedScan.budgetExhausted<<"\n";
            return 28;
        }
        const auto rootBlock=std::find_if(focusedScan.blocks.begin(),focusedScan.blocks.end(),[](const auto& block){return block.first==0x63000;});
        if(rootBlock==focusedScan.blocks.end()||rootBlock->second.size()!=768)return 29;
        // Intermediate graph noise must not exhaust the evidence budget before
        // a deeper layout can be reached. Roots and exact layout records remain.
        put(0x63000,std::uint64_t(0x64000));put(0x64000,std::uint64_t(0x65000));put(0x65000,std::uint64_t(0x5b000));
        ebon::NetworkScanLimits focusedLimits;focusedLimits.maxBytes=1600;focusedLimits.retainGraph=false;
        const auto lean=runtime.scan({0x63000},mod,1000,{objActor,objClass},{{0x63000,512}},focusedLimits);
        focusedLimits.retainGraph=true;
        const auto noisyGraph=runtime.scan({0x63000},mod,1000,{objActor,objClass},{{0x63000,512}},focusedLimits);
        if(lean.layouts.size()!=1||lean.budgetExhausted||lean.bytes>=lean.traversalBytes||
           !noisyGraph.layouts.empty()||!noisyGraph.budgetReasons.count("bytes"))return 38;
        focusedLimits.retainGraph=false;focusedLimits.maxTraversalBytes=512;
        const auto traversalBound=runtime.scan({0x63000},mod,1000,{objActor,objClass},{{0x63000,512}},focusedLimits);
        if(!traversalBound.budgetReasons.count("traversal_bytes")||traversalBound.traversalBytes!=512)return 39;
        std::cout<<"focused network regression: evidence="<<lean.bytes<<" traversal="<<lean.traversalBytes<<" bytes; deeper layout preserved\n";
        put(0x63000,std::uint64_t(0x5b000));put(0x64000,std::uint64_t(0));put(0x65000,std::uint64_t(0));
        // Cmds has exactly 2*32 readable bytes. Saving count*72 used to
        // silently drop the entire command table at an allocation boundary.
        deniedRanges={{0x5d040,0x5d100}};
        put(0x63000,std::uint64_t(0));put(0x632f8,std::uint64_t(0x5b000));
        const auto exactScan=runtime.scan({0x63000,0x63080},mod,1000,{objActor,objClass},{{0x63000,768},{0x63080,512}});
        deniedRanges.clear();
        if(exactScan.layouts.size()!=1||exactScan.incompleteRecords||exactScan.layouts[0].find("\"owner_pointer_address\"")==std::string::npos)return 33;
        bool savedCommands=false;std::size_t savedBytes=0;
        std::vector<std::pair<std::uint64_t,std::uint64_t>> savedRanges;
        for(const auto& b:exactScan.blocks){
            savedBytes+=b.second.size();savedRanges.push_back({b.first,b.first+b.second.size()});
            if(b.first==0x5d000&&b.second.size()==64)savedCommands=true;
        }
        std::sort(savedRanges.begin(),savedRanges.end());
        for(std::size_t i=1;i<savedRanges.size();++i)if(savedRanges[i-1].second>savedRanges[i].first)return 34;
        if(!savedCommands||savedBytes!=exactScan.bytes||savedBytes>16*1024*1024)return 35;
        put(0x632f8,std::uint64_t(0));
        // Primitive TArray elements refer to Inner FProperty, which is absent
        // from the owner's ordinary ChildProperties chain.
        auto nestedFields=runtimeFields;
        nestedFields[0x5e000]={0x5e000,"Actor.Health.inner","Actor",0,false,4};
        ebon::NetworkRuntimeDecoder nestedRuntime(*this,runtimeClasses,nestedFields,noDeadline);
        put(0x5b078,std::int32_t(4));put(0x5b07c,std::int32_t(4));
        put(0x5c01a,std::uint16_t(3));put(0x5d008,std::uint16_t(2));put(0x5d01c,std::uint8_t(0));
        put(0x5d020,std::uint64_t(0x5e000));put(0x5d034,std::uint16_t(1));put(0x5d03c,std::uint8_t(5));
        put(0x5d05c,std::uint8_t(1));put(0x5d07c,std::uint8_t(1));
        if(nestedRuntime.decodeLayout(0x5b000).find("Actor.Health.inner")==std::string::npos||
           !runtime.decodeLayout(0x5b000).empty())return 37;
        put(0x5b078,std::int32_t(2));put(0x5b07c,std::int32_t(2));put(0x5c01a,std::uint16_t(1));
        put(0x5d01c,std::uint8_t(5));put(0x5d03c,std::uint8_t(1));
        constexpr std::uint64_t graphBase=0x100000;constexpr std::size_t graphNodes=10000;
        // Exercise traversal above the former 8,192-node cap with bounded
        // independent roots, without depending on the pointer-depth limit.
        std::vector<std::uint64_t> graphRoots;std::map<std::uint64_t,unsigned> graphSizes;
        for(std::size_t i=0;i<graphNodes;++i){
            const auto address=graphBase+i*512;put(address,std::uint64_t(0));
            graphRoots.push_back(address);graphSizes[address]=512;
        }
        const auto extendedScan=runtime.scan(graphRoots,mod,10000,{objActor,objClass},graphSizes);
        if(extendedScan.nodes!=graphNodes||extendedScan.budgetExhausted){
            std::cerr<<"extended scan roots="<<extendedScan.roots<<" nodes="<<extendedScan.nodes<<" expected="<<graphNodes<<" exhausted="<<extendedScan.budgetExhausted<<" blocks="<<extendedScan.blocks.size()<<"\n";
            return 30;
        }
        class ZeroMemory final:public ebon::MemoryReader {
        public:
            bool copy(std::uint64_t,void* p,std::size_t n)const override{std::memset(p,0,n);return true;}
            bool executable(std::uint64_t)const override{return false;}
            std::vector<ebon::Region> regions(const ebon::Module&)const override{return {};}
        } zeroMemory;
        ebon::NetworkRuntimeDecoder boundedRuntime(zeroMemory,runtimeClasses,runtimeFields,noDeadline);
        std::vector<std::uint64_t> byteRoots;std::map<std::uint64_t,unsigned> byteSizes;
        for(unsigned i=0;i<257;++i){auto address=0x100000ull+i*65536ull;byteRoots.push_back(address);byteSizes[address]=65536;}
        const auto byteScan=boundedRuntime.scan(byteRoots,mod,10000,{},byteSizes);
        if(byteScan.bytes!=16*1024*1024||!byteScan.budgetExhausted||!byteScan.budgetReasons.count("bytes"))return 36;
        put(objActor+16,objStruct);put(field+64,std::uint64_t(0));put(field+112,std::uint64_t(0));
        put(objExample+16,objEnum);put(objExample+32,std::uint64_t(0));put(objExample+176,std::uint32_t(0));
        // Blueprint function locals live in the UStruct frame, beyond ParmsSize.
        const auto local=field+0x200;
        put(local+8,fieldClass);put(local+40,ids["Extra"]);put(local+56,std::int32_t(1));
        put(local+60,std::int32_t(4));put(local+76,std::int32_t(16));
        put(objExample+16,objFunction);put(objExample+80,local);put(objExample+88,std::int32_t(64));
        put(objExample+182,std::uint16_t(8));
        ebon::Reflection locals(*this,c,mod);
        if(!locals.collect())return 15;
        auto function=std::find_if(locals.types.begin(),locals.types.end(),[](const auto& type){return type.kind=="function";});
        if(function==locals.types.end()||function->size!=64||function->parameterSize!=8||function->fields.size()!=1)return 16;
        put(local+64,std::uint64_t(0x80));
        ebon::Reflection invalidParameter(*this,c,mod);
        if(invalidParameter.collect())return 17;
        if(!invalidParameter.resourceInventoryUsable(1,1,1)||invalidParameter.resourceInventoryUsable())return 32;
        put(objExample+16,objEnum);put(objExample+80,std::uint64_t(0));
        put(objActor+28,std::uint32_t(2147482646));
        ebon::Reflection largeNumber(*this,c,mod);
        if(!largeNumber.collect()||largeNumber.objects.back().name!="Actor_2147482645")return 18;
        put(objActor+28,std::uint32_t(0));
        // GC begins after the inventory was cached. Live SDK traversal must
        // refresh it, exclude the destroyed type and prune dependent layouts.
        auto liveConfig=c;liveConfig.values["sdk_live_capture"]="1";
        liveConfig.values["sdk_max_invalid_type_percent"]="25";
        for(int i=6;i<30;++i){
            const auto object=0x65000ull+(std::uint64_t)(i-6)*0x100;
            put(items+(std::uint64_t)i*24,object);put(object,std::uint64_t(0x10800));
            put(object+12,std::int32_t(i));put(object+16,objClass);put(object+24,ids["Extra"]);
            put(object+28,std::uint32_t(i));put(object+80,field);put(object+88,std::int32_t(16));
        }
        put(array+36,std::int32_t(30));put(0x65000+64,objExample);
        put(0x65100+16,objFunction);put(0x65100+32,objExample);
        put(0x65200+88,std::int32_t(1)); // Correct fields cannot fit this owner.
        ebon::SnapshotReader livePages(*this);ebon::Reflection live(livePages,liveConfig,mod);
        live.onProgress=[&](const char* phase,std::uint64_t done,std::uint64_t){
            if(std::string(phase)=="types"&&!done)put(objExample+8,std::uint32_t(0x8000));
        };
        if(!live.collect()||live.excludedTypes.size()!=4||!live.errors.empty()||!live.propertyCount)return 37;
        for(const auto& type:live.types)if(type.address==objExample||type.address==0x65000||type.address==0x65100||type.address==0x65200)return 38;
        live.write(output/"live_mutation",true);
        auto excessiveConfig=liveConfig;excessiveConfig.values["sdk_max_invalid_type_percent"]="0";
        ebon::Reflection excessive(*this,excessiveConfig,mod);
        if(excessive.collect()||excessive.errors.empty())return 39;
        put(objExample+8,std::uint32_t(0));put(array+36,std::int32_t(6));
        put(field+120,objClass);
        std::string pointerTarget;std::optional<unsigned> pointerMask;
        const auto pointerDetail=r.propertyDetail(field,"ObjectProperty",8,pointerTarget,pointerMask,0);
        if(pointerTarget!="Class"||pointerDetail.find("target_offset")==std::string::npos)return 19;
        put(field+128,objStruct); // Adjacent pointer must not create ambiguous object target semantics.
        pointerTarget.clear();r.propertyDetail(field,"ObjectProperty",8,pointerTarget,pointerMask,0);
        if(pointerTarget!="Class")return 42;
        put(field+128,std::uint64_t(0));
        put(field+120,std::uint64_t(0));
        const std::uint8_t boolMetadata[]={1,0,4,4};
        putBytes(nextField+120,boolMetadata,sizeof(boolMetadata));
        std::string boolTarget;std::optional<unsigned> boolMask;
        const auto boolDetail=r.propertyDetail(nextField,"BoolProperty",1,boolTarget,boolMask,0);
        if(boolMask!=4u||boolDetail.find("byte_mask")==std::string::npos)return 20;
        put(field+128,local);put(local+16,field);
        put(local+56,std::int32_t(1));put(local+60,std::int32_t(4));
        std::string arrayTarget;std::optional<unsigned> arrayMask;
        std::vector<ebon::NetworkRuntimeField> innerFields;
        const auto arrayDetail=r.propertyDetail(field,"ArrayProperty",16,arrayTarget,arrayMask,0,&innerFields,"Actor.Values","Actor");
        if(arrayDetail.find("\"inner_offset\":128")==std::string::npos||arrayDetail.find("IntProperty")==std::string::npos||
           arrayDetail.find("\"address\":"+ebon::quote(ebon::hex(local)))==std::string::npos||innerFields.size()!=1||
           innerFields[0].address!=local||innerFields[0].path!="Actor.Values.inner")return 21;
        put(field+128,std::uint64_t(0));
        // UE variants with the compact FField owner use Next/Name at 0x18/0x20
        // and FProperty metadata at 0x30. Verify across several independent types.
        std::uint8_t compactHeader[128]{};putBytes(field,compactHeader,sizeof(compactHeader));
        put(field+8,fieldClass);put(field+32,ids["Health"]);
        put(field+48,std::int32_t(1));put(field+52,std::int32_t(4));put(field+68,std::int32_t(0));
        for(const auto type:{objClass,objStruct,objFunction,objEnum,objActor})put(type+80,field);
        ebon::Reflection compact(*this,c,mod);
        if(!compact.collect()||compact.layout().fieldNext!=24||compact.layout().fieldName!=32||
           compact.layout().propArray!=48||compact.propertyCount<4)return 14;
        // Pointer-shaped noise must be rejected from buffered headers, without
        // tens of thousands of VirtualQuery/RPM calls as in the old scanner.
        for(std::uint64_t at=0x70000;at<0xB0000;at+=8)put(at,std::uint64_t(0x00007f0012340000));
        regionSpan=0xA0000;mod.size=regionSpan;
        copyCalls=regionCalls=invalidReads=0;
        ebon::Reflection noisy(*this,autoConfig,mod);
        if(!noisy.discover()||invalidReads>100||copyCalls>5000)return 5;
        const auto discoveryPasses=regionCalls;
        if(!noisy.collect()||regionCalls!=discoveryPasses)return 6;
        // A header can start in the last eight bytes of a batch.
        const auto crossing=module+256*1024-8;
        std::uint8_t poolHeader[24]{};copy(pool,poolHeader,sizeof(poolHeader));
        putBytes(crossing,poolHeader,sizeof(poolHeader));put(pool+16,std::uint64_t(0));
        ebon::Reflection boundary(*this,autoConfig,mod);
        if(!boundary.discover()||boundary.poolAddress()!=crossing)return 7;
        // Cancellation is checked INSIDE a discovery pass, after its first batch.
        ebon::Reflection timed(*this,autoConfig,mod);
        timed.onProgress=[&](const char*,std::uint64_t done,std::uint64_t){if(done>=256*1024)timed.setDeadline(GetTickCount64()-1);};
        bool stopped=false;
        try{timed.discover();}catch(const std::exception& e){stopped=std::string(e.what()).find("deadline")!=std::string::npos;}
        if(!stopped)return 8;
        // A failed discovery must preserve its reason and produce a state file,
        // without empty objects/types files being mistaken for a completed dump.
        put(crossing+16,std::uint64_t(0));
        ebon::Reflection missing(*this,autoConfig,mod);
        if(missing.discover()||missing.errors.empty())return 9;
        const auto failed=output/"missing";
        missing.writeState(failed,false,"waiting");
        std::ifstream ms(failed/"dump_state.json",std::ios::binary);
        const std::string failure((std::istreambuf_iterator<char>(ms)),{});
        if(failure.find("found 0")==std::string::npos||failure.find("\"status\":\"waiting\"")==std::string::npos||
           std::filesystem::exists(failed/"objects.jsonl")||std::filesystem::exists(failed/"types.jsonl"))return 10;
        // Multiple field reads on one page share a single underlying query/read.
        ebon::SnapshotReader cached(*this);
        copyCalls=0;
        std::uint32_t blockIndex=0,byteCursor=0;
        if(!cached.get(pool+8,blockIndex)||!cached.get(pool+12,byteCursor)||copyCalls!=1)return 11;
        put(pool+12,std::uint32_t(12345));
        if(!cached.get(pool+12,byteCursor)||byteCursor==12345)return 12;
        cached.reset();
        if(!cached.get(pool+12,byteCursor)||byteCursor!=12345)return 13;
        if(!networkVersionSelfTest(*this))return 37;
        std::cout<<"discovery regressions: noise filter, single discovery, batch overlap, deadline, failure state, snapshot cache and NetworkVersion evidence passed\n";
        return 0;
    }
};

int main(int argc,char** argv){
    if(argc==2&&std::strcmp(argv[1],"--network-version-selftest")==0){Snapshot s;return networkVersionSelfTest(s,&std::cout)?0:37;}
    auto out=std::filesystem::temp_directory_path()/"EbonReflectionSelfTest";std::error_code ec;std::filesystem::remove_all(out,ec);Snapshot s;int result=s.run(out);std::filesystem::remove_all(out,ec);std::cout<<"synthetic reflection test="<<result<<"\n";return result;
}
