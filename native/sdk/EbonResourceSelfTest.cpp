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
        Fixture longMemory;
        ebon::Reflection longReflection(longMemory,config,{Fixture::module,0x1000,L"fixture",L"fixture"});
        require(longReflection.collect(),"long path seed failed");
        longMemory.sceneResources(longReflection);
        longReflection.objects[7].path="/Engine/Transient."+std::string(290,'W')+".QuestRuntime";
        ebon::resources::Dumper dumper(longMemory,longReflection,config,output/"long-path");
        require(dumper.run(),"long widget Outer path prevented capture");
        require(contents(output/"long-path"/"resource_manifest.jsonl").find("data/_long/object_")!=std::string::npos,"long path did not use a short filename");
    }
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
