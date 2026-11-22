#pragma once
#include "../sdk/ebon_reflection.hpp"

namespace ebon::rpc_samples {
struct SyncArray {std::uint64_t data=0;std::int32_t count=0,capacity=0;};
struct SyncName {std::uint32_t index=0,number=0;};
struct ReplicatedPropertiesLayout {
    bool verified=false;std::int32_t containerSize=0,properties=-1,owner=-1,module=-1,propertySize=0,name=-1,data=-1,compressed=-1;
    unsigned compressedMask=0;
};
inline const Property* syncField(const Type* type,const char* name){if(type)for(const auto& f:type->fields)if(f.name==name)return &f;return nullptr;}
inline bool syncFieldMatches(const Property* f,const char* type,int offset,int size){
    return f&&f->type==type&&f->offset==offset&&f->element==size&&f->array==1;
}
inline bool syncStructArray(const Property* p,const char* path,int size){
    return p&&p->type=="ArrayProperty"&&p->element==16&&p->array==1&&
        p->detail.find("\"inner\":{\"type\":\"StructProperty\",\"size\":"+std::to_string(size)+",")!=std::string::npos&&
        p->detail.find("\"target\":"+quote(path))!=std::string::npos;
}
inline ReplicatedPropertiesLayout replicatedPropertiesLayout(const Type* function,const Type* container,const Type* parent,const Type* property){
    if(!function||!container||!parent||!property||!function->complete||!container->complete||!parent->complete||!property->complete||
       function->kind!="function"||function->size!=16||function->parameterSize!=16||function->fields.size()!=1||
       container->path!="/Script/HTGame.HTReplicatedObjectPropertyContainer"||container->size!=96||container->super!=parent->path||
       parent->path!="/Script/HottaFramework.HottaReplicatedObjectPropertyContainer"||parent->size!=32||
       property->path!="/Script/HottaFramework.HottaReplicatedObjectProperty"||property->size!=32)return {};
    const auto array=syncField(function,"InObjectPropertyContainerArray"),properties=syncField(parent,"Properties"),owner=syncField(parent,"Owner");
    const auto module=syncField(container,"GameplayModuleId"),name=syncField(property,"Name"),data=syncField(property,"Datas"),compressed=syncField(property,"bCompressed");
    if(!syncFieldMatches(array,"ArrayProperty",0,16)||!syncStructArray(array,container->path.c_str(),96)||
       !syncFieldMatches(properties,"ArrayProperty",8,16)||!syncStructArray(properties,property->path.c_str(),32)||
       !syncFieldMatches(owner,"ObjectProperty",24,8)||owner->target!="/Script/CoreUObject.Object"||
       !syncFieldMatches(module,"NameProperty",84,8)||!syncFieldMatches(name,"NameProperty",0,8)||
       !syncFieldMatches(data,"ArrayProperty",8,16)||data->detail.find("\"inner\":{\"type\":\"ByteProperty\"")==std::string::npos||
       !syncFieldMatches(compressed,"BoolProperty",24,1)||!compressed->boolMask||!*compressed->boolMask)return {};
    return {true,96,8,24,84,32,0,8,24,*compressed->boolMask};
}
inline std::string syncHex(const std::uint8_t* bytes,std::size_t size){
    static const char digits[]="0123456789abcdef";std::string out;out.reserve(size*2);
    for(std::size_t i=0;i<size;++i){out+=digits[bytes[i]>>4];out+=digits[bytes[i]&15];}return out;
}
inline bool syncReadArray(const MemoryReader& memory,std::uint64_t address,std::size_t stride,std::int32_t limit,SyncArray& array){
    if(!memory.get(address,array)||array.count<0||array.count>limit||array.capacity<array.count||array.capacity>limit*4)return false;
    return !array.count||(array.data>=0x10000&&array.data<=UINT64_MAX-(std::uint64_t)array.count*stride);
}
inline bool syncArrayUnchanged(const MemoryReader& memory,std::uint64_t address,const SyncArray& before){
    SyncArray after;return memory.get(address,after)&&after.data==before.data&&after.count==before.count&&after.capacity==before.capacity;
}
inline std::string syncResolveName(const MemoryReader& memory,std::uint64_t pool,const Layout* layout,const SyncName& name){
    if(!pool||!layout)return {};
    const auto& l=*layout;const auto index=name.index>>l.nameBits,mask=(1u<<l.nameBits)-1;
    std::uint32_t current=0,cursor=0;std::uint64_t block=0;
    if(!memory.get(pool+l.poolBlock,current)||current>=8192||index>current||!memory.get(pool+l.poolCursor,cursor)||
       !memory.get(pool+l.poolBlocks+index*8,block)||block<0x10000)return {};
    const auto offset=(name.index&mask)*l.nameStride;std::uint16_t header=0;
    if(!memory.get(block+offset,header))return {};const auto length=header>>l.nameShift;const bool wide=(header&l.nameWide)!=0;
    const auto count=(std::size_t)length*(wide?2:1),end=offset+l.nameHeader+count;
    if(length<1||length>l.maxNameLength||end>((std::uint64_t)1<<l.nameBits)*l.nameStride||(index==current&&end>cursor))return {};
    std::string text;
    if(wide){std::wstring value(length,L'\0');if(!memory.copy(block+offset+l.nameHeader,value.data(),count))return {};try{text=utf8(value);}catch(...){return {};}}
    else {text.resize(length);if(!memory.copy(block+offset+l.nameHeader,text.data(),count))return {};}
    for(const auto c:text)if((unsigned char)c<32||(unsigned char)c==127)return {};
    if(name.number)text+="_"+std::to_string(name.number-1);return text;
}
inline std::string syncNameJson(const MemoryReader& memory,std::uint64_t pool,const Layout* layout,const SyncName& name){
    const auto text=syncResolveName(memory,pool,layout,name);
    return "{\"index\":"+std::to_string(name.index)+",\"number\":"+std::to_string(name.number)+",\"text\":"+(text.empty()?"null":quote(text))+"}";
}
inline bool captureReplicatedProperties(const MemoryReader& memory,std::uint64_t parameters,const ReplicatedPropertiesLayout& layout,
                                        std::uint64_t object,std::uint64_t function,std::uint64_t sample,std::uint64_t tick,
                                        std::size_t maxPayload,std::uint64_t pool,const Layout* names,const char* digest,std::string& output){
    if(!layout.verified||layout.containerSize!=96||layout.propertySize!=32||layout.properties!=8||layout.owner!=24||
       layout.module!=84||layout.name!=0||layout.data!=8||layout.compressed!=24||!layout.compressedMask||parameters<0x10000||maxPayload>8*1024*1024)return false;
    SyncArray containers;if(!syncReadArray(memory,parameters,96,65536,containers))return false;
    constexpr std::size_t maxContainers=128,maxProperties=2048;std::size_t used=0,propertyCount=0;bool truncated=false;
    std::string rows="[";
    const auto takeContainers=std::min<std::size_t>((std::size_t)containers.count,maxContainers);
    for(std::size_t i=0;i<takeContainers;++i){const auto address=containers.data+i*96;
        SyncArray properties;SyncName module;std::uint64_t owner=0;
        if(!syncReadArray(memory,address+layout.properties,32,65536,properties)||!memory.get(address+layout.module,module)||!memory.get(address+layout.owner,owner))return false;
        if(i)rows+=',';rows+="{\"index\":"+std::to_string(i)+",\"owner_address\":"+quote(ebon::hex(owner))+
            ",\"gameplay_module\":"+syncNameJson(memory,pool,names,module)+",\"properties_declared\":"+std::to_string(properties.count)+",\"properties\":[";
        const auto takeProperties=std::min<std::size_t>((std::size_t)properties.count,maxProperties-propertyCount);
        for(std::size_t j=0;j<takeProperties;++j){const auto at=properties.data+j*32;SyncName name;SyncArray data;std::uint8_t compressed=0;
            if(!memory.get(at+layout.name,name)||!syncReadArray(memory,at+layout.data,1,16*1024*1024,data)||!memory.get(at+layout.compressed,compressed))return false;
            const auto take=std::min<std::size_t>((std::size_t)data.count,maxPayload-used);std::vector<std::uint8_t> bytes(take);
            if(take&&!memory.copy(data.data,bytes.data(),take))return false;
            if(!syncArrayUnchanged(memory,at+layout.data,data))return false;
            used+=take;const bool partial=take!=(std::size_t)data.count;truncated|=partial;
            if(j)rows+=',';rows+="{\"name\":"+syncNameJson(memory,pool,names,name)+",\"compressed\":"+((compressed&layout.compressedMask)?"true":"false")+
                ",\"declared_payload_bytes\":"+std::to_string(data.count)+",\"captured_payload_hex\":"+quote(syncHex(bytes.data(),bytes.size()))+
                ",\"truncated\":"+(partial?"true":"false")+"}";
        }
        propertyCount+=takeProperties;const bool partial=takeProperties!=(std::size_t)properties.count;truncated|=partial;
        if(!syncArrayUnchanged(memory,address+layout.properties,properties))return false;
        rows+="],\"properties_truncated\":"+std::string(partial?"true":"false")+"}";
    }
    truncated|=takeContainers!=(std::size_t)containers.count;if(!syncArrayUnchanged(memory,parameters,containers))return false;
    output="{\"kind\":\"SendReplicatedObjectPropertyArrayToClient\",\"function\":\"/Script/HTGame.HTPlayerState.SendReplicatedObjectPropertyArrayToClient\",\"direction\":\"client\",\"capture_stage\":\"ProcessEvent_parameters\",\"wire_packet_bytes_captured\":false,\"module_sha256\":"+quote(digest)+
        ",\"sample_id\":"+std::to_string(sample)+",\"tick_ms\":"+std::to_string(tick)+",\"object_address\":"+quote(ebon::hex(object))+",\"function_address\":"+quote(ebon::hex(function))+
        ",\"containers_declared\":"+std::to_string(containers.count)+",\"containers_captured\":"+std::to_string(takeContainers)+",\"properties_captured\":"+std::to_string(propertyCount)+
        ",\"captured_payload_bytes\":"+std::to_string(used)+",\"truncated\":"+(truncated?"true":"false")+",\"compressed_payloads_decoded\":false,\"map_coverage_verified\":false,\"containers\":"+rows+"]}";
    return true;
}
} // namespace ebon::rpc_samples
