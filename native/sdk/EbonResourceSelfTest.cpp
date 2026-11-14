// Exercises resource values and native sparse containers without a game process.
#include "../dumper/ebon_resource_dump.hpp"
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

class Fixture final:public ebon::MemoryReader {
    static constexpr std::uint64_t base=0x10000;
    std::vector<std::uint8_t> bytes_=std::vector<std::uint8_t>(0x200000);
    std::map<std::string,std::uint32_t> names_;
    std::map<std::string,std::uint64_t> fieldClasses_;
    std::uint64_t nextField_=0x90000,nextString_=0x150000;
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
        for(const auto* type:{"IntProperty","BoolProperty","StrProperty","ArrayProperty","StructProperty","MapProperty","NameProperty","TextProperty","ObjectProperty","DoubleProperty"}){
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
    const auto table=contents(output/"complete"/"data"/"table_100.jsonl");
    for(const auto* expected:{"\"row\":\"CharacterA\"","\"row\":\"CharacterB\"","\"HitPoints\":123",
                             "\"HitPoints\":456","\"Title\":\"角色一\"","\"Enabled\":true","\"Enabled\":false",
                             "\"Scores\":[7,9]","\"Nested\":{\"Rank\":5,\"Ready\":true}",
                             "\"Lookup\":[{\"key\":\"Level\",\"value\":10},{\"key\":\"Rarity\",\"value\":5}]",
                             "FText native layout unverified","candidate only"})
        require(table.find(expected)!=table.npos,"actual table value was missing or incorrect");
    require(table.find("invalid FName")==table.npos,"a free sparse slot was serialized as a row");
    const auto asset=contents(output/"complete"/"data"/"asset_101.jsonl");
    require(asset.find("/Engine/Transient.")!=asset.npos&&asset.find("\"PoolGuaranteeCount\":90")!=asset.npos&&
            asset.find("\"is_default\":false")!=asset.npos,"transient DataAsset values were missing");
    require(asset.find("\"InheritedSetting\":314")!=asset.npos,"empty intermediate class lost ancestor values");
    for(const auto& category:ebon::resources::categoryNames())
        require(std::filesystem::exists(output/"complete"/category/"resources.jsonl"),"category index directory is missing");
    require(contents(output/"complete"/"characters"/"resources.jsonl").find("table_100.jsonl")!=std::string::npos,"character table routing failed");
    require(contents(output/"complete"/"gacha"/"resources.jsonl").find("asset_101.jsonl")!=std::string::npos,"transient lottery routing failed");
    require(contents(output/"complete"/"character_skins"/"resources.jsonl").find("table_100.jsonl")!=std::string::npos,"fashion table routing failed");
    require(contents(output/"complete"/"characters"/"data"/"table_100.jsonl")==table,"category table body differs from canonical values");
    require(contents(output/"complete"/"character_skins"/"data"/"table_100.jsonl")==table,"second category table body was missing");
    require(contents(output/"complete"/"gacha"/"data"/"asset_101.jsonl")==asset,"category asset body was missing");
    const auto gachaIndex=contents(output/"complete"/"gacha"/"resources.jsonl");
    require(gachaIndex.find("\"file\":\"gacha/data/asset_101.jsonl\"")!=gachaIndex.npos&&
            gachaIndex.find("\"values\"")==gachaIndex.npos,"category index embeds values or points to the wrong body");
    {
        const auto conflict=output/"body-conflict";
        std::filesystem::create_directories(conflict/"characters"/"data");
        ebon::save(conflict/"characters"/"data"/"table_100.jsonl","{\"keep\":\"existing body\"}\n");
        ebon::resources::Dumper dumper(memory,reflection,config,conflict);
        require(!dumper.run(),"existing category body was overwritten");
        require(contents(conflict/"characters"/"data"/"table_100.jsonl")=="{\"keep\":\"existing body\"}\n","body conflict changed existing bytes");
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
    require(!std::filesystem::exists(output/"corrupt"/"data"/"table_100.jsonl"),"corrupt row map produced a table file");
    require(contents(output/"corrupt"/"unresolved.jsonl").find("RowMap layout not verified")!=std::string::npos,"corrupt table diagnostics were missing");
    memory.stringResource(reflection);
    {
        ebon::resources::Dumper dumper(memory,reflection,config,output/"text-candidates");
        require(dumper.run(),"validated StringTable candidate fixture failed");
    }
    const auto text=contents(output/"text-candidates"/"data"/"text_102.jsonl");
    require(text.find("\"translations_verified\":false")!=text.npos&&text.find("inferred_candidates")!=text.npos&&
            text.find("\"key\":\"CharacterA.Name\"")!=text.npos&&text.find("角色名称")!=text.npos&&
            text.find("角色说明")!=text.npos,"StringTable candidates were missing or claimed verified translations");
    require(contents(output/"text-candidates"/"resource_state.json").find("\"text_entries\":2")!=std::string::npos,"StringTable sparse holes produced incorrect entry counts");
    require(contents(output/"text-candidates"/"localization"/"data"/"text_102.jsonl")==text,"StringTable body was not published separately");
    memory.sceneResources(reflection);
    {
        ebon::resources::Dumper dumper(memory,reflection,config,output/"scene");
        require(dumper.run(),"scene and script objects were excluded from resources");
    }
    require(contents(output/"scene"/"resource_state.json").find("\"resources\":7")!=std::string::npos,"scene resource count was incorrect");
    require(contents(output/"scene"/"data"/"object_203.jsonl").find("\"RelativeLocation\":{\"X\":1.25,\"Y\":-2.5,\"Z\":3.75}")!=std::string::npos,"reflected scene coordinates were missing");
    require(contents(output/"scene"/"data"/"object_205.jsonl").find("\"is_default\":true")!=std::string::npos,"scene CDO lost its default marker");
    require(contents(output/"scene"/"references.jsonl").find("RootComponent")!=std::string::npos,"scene object reference edges were missing");
    require(contents(output/"scene"/"scripts"/"resources.jsonl").find("object_204.jsonl")!=std::string::npos,"Lua asset was not routed to scripts");
    require(contents(output/"scene"/"scripts"/"data"/"object_204.jsonl")==contents(output/"scene"/"data"/"object_204.jsonl"),"script body differs from original values");
    require(contents(output/"scene"/"maps"/"data"/"object_203.jsonl")==contents(output/"scene"/"data"/"object_203.jsonl"),"scene body was not published separately");
    require(!std::filesystem::exists(output/"scene"/"data"/"object_206.jsonl"),"Evaluation class was incorrectly selected as Lua");
    require(contents(output/"scene"/"data"/"object_207.jsonl").find("\"CurrentQuestId\":5150")!=std::string::npos,"non-DataAsset gameplay manager was excluded");
    {
        auto focused=config;focused.values["resource_game_objects"]="0";
        ebon::resources::Dumper dumper(memory,reflection,focused,output/"scene-focused");
        require(dumper.run(),"disabling gameplay objects prevented scene capture");
    }
    require(!std::filesystem::exists(output/"scene-focused"/"data"/"object_207.jsonl"),"gameplay instance switch was ignored");
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
    std::cout<<"Resource self-test passed: table values, sparse holes, corrupt bitmaps, transient assets, scene coordinates, defaults, Lua asset values, references, categories, text candidates and budgets\n";
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
