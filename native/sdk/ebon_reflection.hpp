#pragma once
#include "../common/ebon_common.hpp"
#include <optional>
#include "ebon_network_runtime.hpp"

namespace ebon {
struct Layout {
    std::int64_t poolBlocks=16,poolBlock=8,poolCursor=12,nameShift=6,nameWide=1,nameHeader=2,nameStride=2,nameBits=16;
    std::int64_t chunks=16,numElements=36,maxElements=32,numChunks=44,maxChunks=40,itemStride=24,itemObject=0,elementsPerChunk=65536;
    std::int64_t objIndex=12,objClass=16,objName=24,objOuter=32,objFlags=8,nameNumber=4;
    std::int64_t structSuper=64,structSize=88,structFields=80,fieldClass=8,fieldNext=32,fieldName=40,fieldClassName=0;
    std::int64_t propArray=56,propElement=60,propFlags=64,propOffset=76,propTarget=-1,boolMask=-1;
    std::int64_t functionFlags=176,functionParmsSize=182,functionNative=216,enumNames=64,enumStride=16;
    std::int64_t functionRpcId=-1,functionRpcResponseId=-1,propertyRepIndex=-1,propertyRepCondition=-1,propertyRepNotify=-1;
    std::int64_t maxObjects=2000000,maxFields=4096,maxNameLength=1023,maxTypeSize=16777216,scanMb=512;
    explicit Layout(const Config& c) {
#define EBON_LAYOUT(member,key) member=c.number(key,member)
        EBON_LAYOUT(poolBlocks,"name_pool_blocks_offset");EBON_LAYOUT(poolBlock,"name_pool_current_block_offset");EBON_LAYOUT(poolCursor,"name_pool_current_byte_cursor_offset");
        EBON_LAYOUT(nameShift,"name_entry_length_shift");EBON_LAYOUT(nameWide,"name_entry_wide_mask");EBON_LAYOUT(nameHeader,"name_entry_header_size");EBON_LAYOUT(nameStride,"name_entry_stride");EBON_LAYOUT(nameBits,"name_block_bits");
        EBON_LAYOUT(chunks,"object_array_chunks_offset");EBON_LAYOUT(numElements,"object_array_num_elements_offset");EBON_LAYOUT(maxElements,"object_array_max_elements_offset");EBON_LAYOUT(numChunks,"object_array_num_chunks_offset");EBON_LAYOUT(maxChunks,"object_array_max_chunks_offset");
        EBON_LAYOUT(itemStride,"object_item_stride");EBON_LAYOUT(itemObject,"object_item_object_offset");EBON_LAYOUT(elementsPerChunk,"object_array_elements_per_chunk");
        EBON_LAYOUT(objIndex,"object_index_offset");EBON_LAYOUT(objClass,"object_class_offset");EBON_LAYOUT(objName,"object_name_offset");EBON_LAYOUT(objOuter,"object_outer_offset");EBON_LAYOUT(nameNumber,"fname_number_offset");
        EBON_LAYOUT(objFlags,"object_flags_offset");
        EBON_LAYOUT(structSuper,"struct_super_offset");EBON_LAYOUT(structSize,"struct_size_offset");EBON_LAYOUT(structFields,"struct_child_properties_offset");EBON_LAYOUT(fieldClass,"field_class_offset");EBON_LAYOUT(fieldNext,"field_next_offset");EBON_LAYOUT(fieldName,"field_name_offset");EBON_LAYOUT(fieldClassName,"field_class_name_offset");
        EBON_LAYOUT(propArray,"property_array_dim_offset");EBON_LAYOUT(propElement,"property_element_size_offset");EBON_LAYOUT(propFlags,"property_flags_offset");EBON_LAYOUT(propOffset,"property_offset_offset");EBON_LAYOUT(propTarget,"property_target_offset");EBON_LAYOUT(boolMask,"bool_field_mask_offset");
        EBON_LAYOUT(functionFlags,"function_flags_offset");EBON_LAYOUT(functionParmsSize,"function_params_size_offset");EBON_LAYOUT(functionNative,"function_native_offset");EBON_LAYOUT(enumNames,"enum_names_offset");EBON_LAYOUT(enumStride,"enum_item_stride");
        EBON_LAYOUT(functionRpcId,"function_rpc_id_offset");EBON_LAYOUT(functionRpcResponseId,"function_rpc_response_id_offset");
        EBON_LAYOUT(propertyRepIndex,"property_rep_index_offset");EBON_LAYOUT(propertyRepCondition,"property_rep_condition_offset");EBON_LAYOUT(propertyRepNotify,"property_rep_notify_offset");
        EBON_LAYOUT(maxObjects,"max_objects");EBON_LAYOUT(maxFields,"max_fields_per_type");EBON_LAYOUT(maxNameLength,"max_name_length");EBON_LAYOUT(maxTypeSize,"max_type_size");EBON_LAYOUT(scanMb,"auto_scan_max_mb");
#undef EBON_LAYOUT
        const std::int64_t mandatory[]={poolBlocks,poolBlock,poolCursor,objIndex,objClass,objName,objOuter,chunks,numElements,maxElements,numChunks,maxChunks,itemObject,structSuper,structSize,structFields,fieldClass,fieldNext,fieldName,fieldClassName,propArray,propElement,propFlags,propOffset,enumNames};
        for(auto n:mandatory)if(n<0||n>4096)throw std::runtime_error("layout offset outside supported 0..4096");
        for(auto n:{objFlags,functionRpcId,functionRpcResponseId,propertyRepIndex,propertyRepCondition,propertyRepNotify})
            if(n< -1||n>4096)throw std::runtime_error("network layout offset outside supported -1..4096");
        if(nameShift<0||nameShift>15||nameHeader<2||nameHeader>16||nameStride<1||nameStride>8||nameBits<12||nameBits>20||nameNumber<4||nameNumber>12||itemStride<8||itemStride>128||elementsPerChunk<256||elementsPerChunk>1048576||maxObjects<1||maxObjects>10000000||maxFields<1||maxFields>65536||maxNameLength<4||maxNameLength>4096||maxTypeSize<1||maxTypeSize>268435456||scanMb<1||scanMb>4096||enumStride<16||enumStride>64)throw std::runtime_error("layout bounds invalid");
    }
};
struct Object {std::int32_t index=0;std::uint64_t address=0,klass=0;std::string name,className,path;};
struct Property {
    std::string name,type,target,role;
    std::string detail="{}";
    std::vector<NetworkRuntimeField> innerFields;
    std::uint64_t address=0,flags=0;
    std::int32_t offset=-1,element=-1,array=-1;
    std::optional<unsigned> boolMask;
    std::optional<std::uint16_t> repIndex;
    std::optional<std::uint8_t> repCondition;
    std::string repNotify;
};
struct Type {
    std::string kind,name,path,super;
    std::uint64_t address=0;
    std::optional<std::int32_t> size;
    std::optional<std::uint16_t> parameterSize;
    std::optional<std::uint32_t> alignment;
    std::optional<std::uint64_t> flags,nativeRva;
    std::optional<std::uint16_t> rpcId,rpcResponseId;
    std::vector<Property> fields;
    std::vector<std::pair<std::string,std::int64_t>> enumValues;
    bool complete=true;
};
class Reflection {
    const MemoryReader& mem_;const Config& config_;Module module_;Layout l_;
    std::uint64_t pool_=0,objects_=0,chunkTable_=0;std::int32_t count_=0,block_=0,cursor_=0;
    bool layoutVerified_=false,propertyLayoutVerified_=false;
    bool propertyLayoutInferred_=false;
    ULONGLONG deadline_=0;
    mutable std::string lastArrayFailure_;
    mutable std::map<std::uint32_t,std::string> names_;
    std::map<std::uint64_t,Object> objectMap_;
public:
    // Called between bounded batches, including while a single discovery pass runs.
    std::function<void(const char*,std::uint64_t,std::uint64_t)> onProgress;
    std::string progressPhase="initializing";
    std::uint64_t progressDone=0,progressTotal=0;
    void report(const char* phase,std::uint64_t done,std::uint64_t total){progressPhase=phase;progressDone=done;progressTotal=total;if(onProgress)onProgress(phase,done,total);}
    void setDeadline(ULONGLONG deadline){deadline_=deadline;}
    void checkDeadline()const {if(deadline_&&GetTickCount64()>=deadline_)throw std::runtime_error("reflection deadline exceeded");}
    std::vector<Object> objects;std::vector<Type> types;std::vector<std::string> errors,unresolved,optionalUnresolved;
    std::string moduleSha256;
    std::vector<std::uint64_t> poolCandidates,arrayCandidates;
    std::vector<std::string> invalidObjectSamples,fieldLayoutProbes;
    std::vector<std::string> runtimeVtables;
    std::vector<std::string> classNetworkTables;
    std::vector<std::string> networkCacheCandidates;
    NetworkRuntimeResult runtimeNetwork;
    std::size_t rpcCount=0,replicatedPropertyCount=0;
    std::size_t nullSlots=0,invalidObjects=0,propertyCount=0;
    explicit Reflection(const MemoryReader& m,const Config& c,Module module):mem_(m),config_(c),module_(std::move(module)),l_(c){}
    const Layout& layout()const{return l_;}
    std::uint64_t poolAddress()const{return pool_;}std::uint64_t objectArrayAddress()const{return objects_;}
    std::int32_t objectCount()const{return count_;}
    bool resourceInventoryUsable(std::size_t minObjects=1000,std::size_t minTypes=1000,
                                 std::size_t minProperties=1000)const {
        return layoutVerified_&&propertyLayoutVerified_&&objects.size()>=minObjects&&
               types.size()>=minTypes&&propertyCount>=minProperties;
    }
    template<class T> bool read(std::uint64_t a,std::int64_t off,T& value)const{return off>=0&&a+(std::uint64_t)off>=a&&mem_.get(a+(std::uint64_t)off,value);}
    bool ptr(std::uint64_t p)const {std::uint64_t first=0;return p>=0x10000&&!(p&7)&&mem_.get(p,first);}
    bool plausible(const std::string& s)const {if(s.empty()||s.size()>16384)return false;for(unsigned char c:s)if(c<32||c==127)return false;return true;}
    std::string entry(std::uint64_t pool,std::uint32_t id)const {
        const auto index=id>>l_.nameBits,mask=(1u<<l_.nameBits)-1;
        std::uint32_t current=0,cursor=0;std::uint64_t block=0;
        if(!read(pool,l_.poolBlock,current)||!read(pool,l_.poolCursor,cursor)||current>=8192||index>current||!read(pool,l_.poolBlocks+index*8,block)||!ptr(block))return {};
        std::uint64_t offset=(id&mask)*l_.nameStride;std::uint16_t h=0;
        if(!mem_.get(block+offset,h))return {};auto len=h>>l_.nameShift;bool isWide=(h&l_.nameWide)!=0;
        if(len<1||len>l_.maxNameLength)return {};std::size_t bytes=(std::size_t)len*(isWide?2:1);auto end=offset+l_.nameHeader+bytes;
        if(end>((std::uint64_t)1<<l_.nameBits)*l_.nameStride||(index==current&&end>cursor))return {};
        if(isWide){std::wstring w(len,L'\0');if(!mem_.copy(block+offset+l_.nameHeader,w.data(),bytes))return {};try {auto s=utf8(w);return plausible(s)?s:std::string();}catch(...){return {};}}
        std::string s(len,'\0');if(!mem_.copy(block+offset+l_.nameHeader,s.data(),bytes)||!plausible(s))return {};try{wide(s);}catch(...){return {};}return s;
    }
    bool validatePool(std::uint64_t p)const {
        if(entry(p,0)!="None")return false;std::uint64_t block=0;if(!read(p,l_.poolBlocks,block))return false;
        std::uint64_t at=0;int valid=0;std::set<std::string> distinct;
        for(int n=0;n<12;++n){std::uint16_t h=0;if(!mem_.get(block+at,h))return false;auto len=h>>l_.nameShift;if(!len||len>l_.maxNameLength)return false;auto text=entry(p,(std::uint32_t)(at/l_.nameStride));if(text.empty())return false;distinct.insert(text);++valid;at+=l_.nameHeader+len*((h&l_.nameWide)?2:1);at=(at+l_.nameStride-1)/l_.nameStride*l_.nameStride;}
        return valid==12&&distinct.size()>=10;
    }
    std::string name(std::uint64_t a,std::int64_t off)const {
        // FName instance numbers near INT32_MAX are used by valid engine objects.
        // An arbitrary decimal cap discarded these objects and their descendants.
        std::uint32_t id=0,number=0;if(!read(a,off,id)||!read(a,off+l_.nameNumber,number))return {};
        auto it=names_.find(id);std::string s;if(it!=names_.end())s=it->second;else{s=entry(pool_,id);names_[id]=s;}
        if(!s.empty()&&number)s+="_"+std::to_string(number-1);return s;
    }
    std::string nameDiagnostic(std::uint64_t address)const {
        std::uint32_t id=0,number=0,current=0,cursor=0;
        read(address,l_.objName,id);read(address,l_.objName+l_.nameNumber,number);
        read(pool_,l_.poolBlock,current);read(pool_,l_.poolCursor,cursor);
        std::uint64_t block=0;
        read(pool_,l_.poolBlocks+(id>>l_.nameBits)*8,block);
        const auto entryAddress=block+(id&((1u<<l_.nameBits)-1))*l_.nameStride;
        std::array<unsigned char,32> bytes{};
        const bool readable=mem_.copy(entryAddress,bytes.data(),bytes.size());
        std::ostringstream result;
        result<<" id="<<hex(id)<<" number="<<number<<" pool_block="<<current<<" cursor="<<cursor
              <<" entry="<<hex(entryAddress)<<" bytes=";
        if(readable)for(auto byte:bytes)result<<std::hex<<std::setw(2)<<std::setfill('0')<<(unsigned)byte;
        else result<<"unreadable";
        return result.str();
    }
    bool objectBasic(std::uint64_t p)const {
        if(!ptr(p))return false;std::uint64_t v=0,klass=0;std::int32_t idx=-1;
        return mem_.get(p,v)&&mem_.executable(v==0?0:([&](){std::uint64_t fn=0;return mem_.get(v,fn)?fn:0;})())&&read(p,l_.objClass,klass)&&ptr(klass)&&read(p,l_.objIndex,idx)&&idx>=0&&idx<l_.maxObjects&&plausible(name(p,l_.objName));
    }
    bool arrayHeader(std::uint64_t p,std::uint64_t& table,std::int32_t& n)const {
        std::int32_t max=0,nc=0,mc=0;
        if(!read(p,l_.chunks,table)||!ptr(table)||!read(p,l_.numElements,n)||!read(p,l_.maxElements,max)||!read(p,l_.numChunks,nc)||!read(p,l_.maxChunks,mc))return false;
        if(n<3||n>l_.maxObjects||max<n||max>100000000||nc<1||nc>mc||mc>16384||(std::int64_t)nc*l_.elementsPerChunk<n||(std::int64_t)(nc-1)*l_.elementsPerChunk>=n)return false;
        std::uint64_t first=0,last=0;return mem_.get(table,first)&&ptr(first)&&mem_.get(table+(nc-1)*8,last)&&ptr(last);
    }
    std::uint64_t objectAt(std::uint64_t table,std::int32_t i)const {
        std::uint64_t chunk=0,obj=0;if(i<0||!mem_.get(table+(i/l_.elementsPerChunk)*8,chunk)||!ptr(chunk)||!read(chunk,(i%l_.elementsPerChunk)*l_.itemStride+l_.itemObject,obj))return 0;return obj;
    }
    bool validateArray(std::uint64_t p)const {
        lastArrayFailure_.clear();std::uint64_t table=0;std::int32_t n=0;if(!arrayHeader(p,table,n)){lastArrayFailure_="invalid chunk/count header";return false;}int seen=0,valid=0,classMeta=0;
        for(int i=0;i<std::min(n,1024);++i){auto obj=objectAt(table,i);if(!obj)continue;++seen;if(!objectBasic(obj))continue;std::int32_t index=-1;std::uint64_t cls=0,meta=0;if(!read(obj,l_.objIndex,index)||index!=i||!read(obj,l_.objClass,cls))continue;++valid;if(read(cls,l_.objClass,meta)&&name(meta,l_.objName)=="Class")++classMeta;if(seen>=64)break;}
        if(seen<3||valid<3||valid*100/seen<80||classMeta*100/valid<80){lastArrayFailure_="samples="+std::to_string(seen)+", valid="+std::to_string(valid)+", Class-meta="+std::to_string(classMeta);return false;}return true;
    }
    void scan(bool forPool) {
        checkDeadline();
        auto all=mem_.regions(module_);
        const std::uint64_t limit=(std::uint64_t)l_.scanMb*1024*1024;
        const char* phase=forPool?"name_pool_scan":"object_array_scan";
        const auto headerBytes=(std::size_t)(forPool?
            std::max({l_.poolBlocks+8,l_.poolBlock+4,l_.poolCursor+4}):
            std::max({l_.chunks+8,l_.numElements+4,l_.maxElements+4,l_.numChunks+4,l_.maxChunks+4}));
        std::uint64_t total=0,scanned=0,failedReads=0;
        std::map<std::uint64_t,bool> firstBlocks;
        for(const auto& r:all)if(!r.executable&&r.writable)total+=r.size;
        const auto progress=[&](){report(phase,scanned,total);};
        progress();
        for(const auto& region:all){
            if(region.executable||!region.writable)continue;
            const auto end=std::min(region.address+region.size,module_.base+module_.size);
            for(std::uint64_t pos=std::max(region.address,module_.base);pos<end;){
                checkDeadline();
                if(scanned>=limit){unresolved.push_back("automatic discovery scan budget exhausted");progress();return;}
                const auto step=(std::size_t)std::min({std::uint64_t(256*1024),end-pos,limit-scanned});
                // Keep the complete header for candidates straddling a batch boundary.
                const auto bytes=(std::size_t)std::min<std::uint64_t>(step+headerBytes-1,end-pos);
                std::vector<unsigned char> data(bytes);
                if(mem_.copy(pos,data.data(),bytes)){
                    for(std::size_t i=(std::size_t)((8-(pos&7))&7);i<step&&i+headerBytes<=bytes;i+=8){
                        if((i&2047)==0){checkDeadline();report(phase,scanned+i,total);}
                        const auto u32=[&](std::int64_t off){std::uint32_t v=0;std::memcpy(&v,data.data()+i+off,4);return v;};
                        const auto u64=[&](std::int64_t off){std::uint64_t v=0;std::memcpy(&v,data.data()+i+off,8);return v;};
                        // Reject noise using the buffered header before ANY pointer read.
                        // The old loop queried/dereferenced every pointer-shaped word.
                        if(forPool){
                            const auto block=u32(l_.poolBlock),cursor=u32(l_.poolCursor);
                            const auto first=u64(l_.poolBlocks);
                            if(block>=8192||cursor>((std::uint64_t)1<<l_.nameBits)*l_.nameStride||
                               (!block&&cursor<6)||first<0x10000||(first&7)||first>=0x0000800000000000ull)continue;
                            auto match=firstBlocks.find(first);
                            if(match==firstBlocks.end()){
                                std::array<unsigned char,20> entry{};
                                std::uint16_t header=0;
                                const bool readable=mem_.copy(first,entry.data(),(std::size_t)l_.nameHeader+4);
                                std::memcpy(&header,entry.data(),sizeof(header));
                                const bool isNone=readable&&(header>>l_.nameShift)==4&&!(header&l_.nameWide)&&
                                    std::memcmp(entry.data()+l_.nameHeader,"None",4)==0;
                                match=firstBlocks.emplace(first,isNone).first;
                            }
                            if(!match->second)continue;
                            if(validatePool(pos+i))poolCandidates.push_back(pos+i);
                        }else{
                            const auto n=u32(l_.numElements),max=u32(l_.maxElements);
                            const auto nc=u32(l_.numChunks),mc=u32(l_.maxChunks);
                            const auto table=u64(l_.chunks);
                            if(n<3||n>(std::uint64_t)l_.maxObjects||max<n||max>100000000||
                               nc<1||nc>mc||mc>16384||(std::uint64_t)nc*l_.elementsPerChunk<n||
                               (std::uint64_t)(nc-1)*l_.elementsPerChunk>=n||table<0x10000||
                               (table&7)||table>=0x0000800000000000ull)continue;
                            if(validateArray(pos+i))arrayCandidates.push_back(pos+i);
                        }
                    }
                }else ++failedReads;
                pos+=step;scanned+=step;progress();
            }
        }
        if(failedReads)unresolved.push_back(std::to_string(failedReads)+" discovery batches were unreadable");
        auto& list=forPool?poolCandidates:arrayCandidates;
        std::sort(list.begin(),list.end());list.erase(std::unique(list.begin(),list.end()),list.end());
    }
    bool discover() {
        checkDeadline();
        errors.clear();unresolved.clear();poolCandidates.clear();arrayCandidates.clear();
        pool_=objects_=chunkTable_=0;layoutVerified_=false;names_.clear();
        const auto np=config_.number("name_pool_rva",0),oa=config_.number("object_array_rva",0);if(np<0||oa<0)throw std::runtime_error("RVA must be nonnegative");
        if(np){pool_=module_.base+np;if((std::uint64_t)np>=module_.size||!validatePool(pool_)){errors.push_back("configured name pool did not validate");return false;}}
        else {scan(true);if(poolCandidates.size()!=1){errors.push_back("name pool discovery requires one validated candidate; found "+std::to_string(poolCandidates.size()));return false;}pool_=poolCandidates.front();}
        names_.clear();
        if(oa){objects_=module_.base+oa;if((std::uint64_t)oa>=module_.size||!validateArray(objects_)){errors.push_back("configured object array did not validate against UObject/Class samples: "+lastArrayFailure_);return false;}}
        else {scan(false);if(arrayCandidates.size()!=1){errors.push_back("object array discovery requires one validated candidate; found "+std::to_string(arrayCandidates.size()));return false;}objects_=arrayCandidates.front();}
        if(!arrayHeader(objects_,chunkTable_,count_)){errors.push_back("object array changed while reading");return false;}layoutVerified_=true;return true;
    }
    std::string path(std::uint64_t obj)const {
        std::vector<std::string> parts;std::set<std::uint64_t> seen;
        for(int depth=0;obj&&depth<64;++depth){if(!seen.insert(obj).second)return {};auto n=name(obj,l_.objName);if(!plausible(n))return {};parts.push_back(n);std::uint64_t next=0;if(!read(obj,l_.objOuter,next))return {};obj=next;}
        if(obj)return {};std::string result;for(auto i=parts.rbegin();i!=parts.rend();++i){if(!result.empty())result+='.';result+=*i;}return result;
    }
    std::string role(std::uint64_t flags)const {if(!(flags&0x80))return "not_parameter";if(flags&0x400)return "return";if(flags&0x100)return flags&0x08000000?"out_ref":"out";if(flags&0x08000000)return flags&2?"const_ref":"inout_ref";return flags&2?"const_in":"in";}
    bool propertySample(std::uint64_t field,const Layout& layout,std::int32_t typeSize)const {
        std::uint64_t cls=0,next=0,flags=0;
        std::int32_t array=0,element=0,offset=0;
        if(!read(field,layout.fieldClass,cls)||!ptr(cls))return false;
        const auto type=name(cls,layout.fieldClassName),fieldName=name(field,layout.fieldName);
        if(type.size()<8||type.substr(type.size()-8)!="Property"||!plausible(fieldName))return false;
        if(!read(field,layout.fieldNext,next)||(next&&!ptr(next))||
           !read(field,layout.propArray,array)||!read(field,layout.propElement,element)||
           !read(field,layout.propOffset,offset)||!read(field,layout.propFlags,flags))return false;
        return array>=1&&array<=65536&&element>=1&&element<=layout.maxTypeSize&&offset>=0&&
               (std::int64_t)offset+(std::int64_t)array*element<=typeSize;
    }
    void inferPropertyLayout() {
        struct Sample{std::uint64_t field;std::int32_t size;};
        std::vector<Sample> samples;
        for(const auto& object:objects){
            if(object.className!="Class"&&object.className!="ScriptStruct")continue;
            std::uint64_t field=0;std::int32_t size=0;
            if(read(object.address,l_.structFields,field)&&field&&ptr(field)&&
               read(object.address,l_.structSize,size)&&size>0&&size<=l_.maxTypeSize)
                samples.push_back({field,size});
            if(samples.size()>=24)break;
        }
        if(samples.size()<4)return;
        const auto score=[&](const Layout& candidate){
            unsigned valid=0;
            for(const auto& sample:samples){checkDeadline();if(propertySample(sample.field,candidate,sample.size))++valid;}
            return valid;
        };
        if(score(l_)*100>=samples.size()*80)return;
        std::vector<Layout> matches;
        fieldLayoutProbes.clear();
        for(const auto next:{24,32})for(const auto array:{48,56}){
            auto candidate=l_;
            candidate.fieldNext=next;candidate.fieldName=next+8;
            candidate.propArray=array;candidate.propElement=array+4;
            candidate.propFlags=array+8;candidate.propOffset=array+20;
            const auto valid=score(candidate);
            fieldLayoutProbes.push_back("next="+hex(next)+" name="+hex(next+8)+" array="+hex(array)+" valid="+std::to_string(valid)+"/"+std::to_string(samples.size()));
            if(valid*100>=samples.size()*80)matches.push_back(candidate);
        }
        if(matches.size()==1){l_=matches.front();propertyLayoutInferred_=true;}
    }
    void readFields(Type& type) {
        std::uint64_t field=0;if(!read(type.address,l_.structFields,field)){type.complete=false;errors.push_back("unreadable ChildProperties: "+type.path);return;}
        std::set<std::uint64_t> seen;
        for(std::int64_t step=0;field&&step<l_.maxFields;++step){if((step&255)==0)checkDeadline();if(!seen.insert(field).second){type.complete=false;errors.push_back("property chain cycle: "+type.path);break;}
            Property p;p.address=field;p.name=name(field,l_.fieldName);std::uint64_t cls=0;
            if(!read(field,l_.fieldClass,cls)||!ptr(cls)||!(p.type=name(cls,l_.fieldClassName)).size()||p.type.size()<8||p.type.substr(p.type.size()-8)!="Property"||!plausible(p.name)||!read(field,l_.propArray,p.array)||!read(field,l_.propElement,p.element)||!read(field,l_.propOffset,p.offset)||!read(field,l_.propFlags,p.flags)||p.array<1||p.array>65536||p.element<1||p.element>l_.maxTypeSize||p.offset<0||(std::int64_t)p.array*p.element>l_.maxTypeSize||(std::int64_t)p.offset+(std::int64_t)p.array*p.element>l_.maxTypeSize){type.complete=false;errors.push_back("property layout failed validation: "+type.path+" at "+hex(field));break;}
            if(type.size&&(std::int64_t)p.offset+(std::int64_t)p.array*p.element>*type.size){type.complete=false;errors.push_back("property extends beyond reflected size: "+type.path+"."+p.name);break;}
            if(type.kind=="function"&&(p.flags&0x80)&&type.parameterSize&&
               (std::int64_t)p.offset+(std::int64_t)p.array*p.element>*type.parameterSize){
                type.complete=false;errors.push_back("parameter extends beyond parameter buffer: "+type.path+"."+p.name);break;
            }
            p.role=type.kind=="function"?role(p.flags):"member";
            if(p.flags&0x20){
                ++replicatedPropertyCount;
                std::uint16_t index=0;std::uint8_t condition=0;
                if(l_.propertyRepIndex>=0&&read(field,l_.propertyRepIndex,index))p.repIndex=index;
                if(l_.propertyRepCondition>=0&&read(field,l_.propertyRepCondition,condition))p.repCondition=condition;
                if(l_.propertyRepNotify>=0){auto notify=name(field,l_.propertyRepNotify);if(notify!="None"&&plausible(notify))p.repNotify=notify;}
            }
            p.detail=propertyDetail(field,p.type,p.element,p.target,p.boolMask,0,&p.innerFields,type.path+"."+p.name,type.path);
            if(l_.propTarget>=0&&(p.type=="StructProperty"||p.type=="ObjectProperty"||p.type=="ClassProperty"||p.type=="EnumProperty")){std::uint64_t target=0;if(read(field,l_.propTarget,target)&&objectMap_.count(target))p.target=objectMap_.at(target).path;}
            if(p.type=="BoolProperty"){if(l_.boolMask>=0){std::uint8_t mask=0;if(read(field,l_.boolMask,mask)&&mask)p.boolMask=mask;}if(!p.boolMask)optionalUnresolved.push_back("BoolProperty mask unavailable: "+type.path+"."+p.name);}
            if((p.type=="StructProperty"||p.type=="ObjectProperty"||p.type=="ClassProperty"||p.type=="EnumProperty")&&p.target.empty())optionalUnresolved.push_back("property target layout unavailable: "+type.path+"."+p.name);
            type.fields.push_back(std::move(p));++propertyCount;
            std::uint64_t next=0;if(!read(field,l_.fieldNext,next)){type.complete=false;errors.push_back("unreadable property Next: "+type.path);break;}field=next;
        }
        if(field&&seen.size()>=(std::size_t)l_.maxFields){type.complete=false;errors.push_back("property chain exceeded configured limit: "+type.path);}
    }
    std::string propertyDetail(std::uint64_t field,const std::string& type,std::int32_t size,
                               std::string& target,std::optional<unsigned>& mask,unsigned depth,
                               std::vector<NetworkRuntimeField>* nested=nullptr,const std::string& fieldPath="",const std::string& fieldOwner="") {
        if(depth>=8)return "{}";
        const auto extra=l_.propOffset+0x2c;
        auto result="{\"type\":"+quote(type)+",\"size\":"+std::to_string(size);
        std::int32_t fieldOffset=0,arrayDim=0;std::uint64_t flags=0;
        if(read(field,l_.propOffset,fieldOffset)&&read(field,l_.propArray,arrayDim)&&read(field,l_.propFlags,flags)&&
           fieldOffset>=0&&fieldOffset<=l_.maxTypeSize&&arrayDim>=1&&arrayDim<=65536){
            result+=",\"address\":"+quote(hex(field))+",\"offset\":"+std::to_string(fieldOffset)+",\"element_size\":"+std::to_string(size)+",\"array_dim\":"+std::to_string(arrayDim)+",\"flags\":"+quote(hex(flags));
            if(nested&&depth)nested->push_back({field,fieldPath,fieldOwner,fieldOffset,false,size,arrayDim});
        }
        const auto targetMatches=[&](const Object& o){
            if(type=="StructProperty")return o.className=="ScriptStruct";
            if(type=="EnumProperty"||type=="ByteProperty")return o.className=="Enum"||o.className=="UserDefinedEnum";
            if(type.find("DelegateProperty")!=std::string::npos)return o.className.find("Function")!=std::string::npos;
            return o.className=="Class"||o.className.find("GeneratedClass")!=std::string::npos;
        };
        if(type=="StructProperty"||type=="EnumProperty"||type=="ByteProperty"||type.find("ObjectProperty")!=std::string::npos||
           type=="ClassProperty"||type=="SoftClassProperty"||type=="InterfaceProperty"||type.find("DelegateProperty")!=std::string::npos){
            std::map<std::string,std::int64_t> matches;
            for(auto offset=extra;offset<=extra+0x20;offset+=8){
                std::uint64_t address=0;
                if(!read(field,offset,address))continue;
                const auto found=objectMap_.find(address);
                if(found==objectMap_.end()||!targetMatches(found->second))continue;
                if(type=="StructProperty"){
                    std::int32_t reflected=0;
                    if(!read(address,l_.structSize,reflected)||reflected!=size)continue;
                }
                matches[found->second.path]=offset;
            }
            if((type=="ClassProperty"||type=="SoftClassProperty")&&matches.size()>1)
                matches.erase("/Script/CoreUObject.Class");
            if(matches.size()==1){target=matches.begin()->first;result+=",\"target\":"+quote(target)+",\"target_offset\":"+std::to_string(matches.begin()->second);}
        }
        if(type=="BoolProperty"){
            std::uint8_t bytes[4]{};
            if(mem_.copy(field+extra,bytes,sizeof(bytes))&&bytes[0]>=1&&bytes[0]<=8&&bytes[1]<bytes[0]&&
               bytes[2]&&(bytes[2]&(bytes[2]-1))==0&&bytes[3]&&(bytes[3]==255||bytes[3]==bytes[2])){
                mask=bytes[3];
                result+=",\"bool_mask\":"+std::to_string(bytes[3])+",\"byte_mask\":"+std::to_string(bytes[2])+",\"byte_offset\":"+std::to_string(bytes[1]);
            }
        }
        const auto child=[&](std::int64_t offset,const char* key){
            std::uint64_t address=0,cls=0,owner=0;
            std::int32_t element=0;
            if(!read(field,offset,address)||!ptr(address)||address==field||!read(address,l_.fieldClass,cls)||
               !read(address,16,owner)||(owner&~std::uint64_t(1))!=field)return std::string("null");
            const auto childType=name(cls,l_.fieldClassName);
            if(childType.size()<8||childType.substr(childType.size()-8)!="Property"||!read(address,l_.propElement,element)||element<1||element>l_.maxTypeSize)return std::string("null");
            std::string childTarget;std::optional<unsigned> childMask;
            return propertyDetail(address,childType,element,childTarget,childMask,depth+1,nested,fieldPath+"."+key,fieldOwner);
        };
        if(type=="ArrayProperty"){
            std::string inner="null";
            for(auto offset=extra;offset<=extra+0x20;offset+=8){
                std::uint64_t address=0,owner=0;
                if(!read(field,offset,address)||!read(address,16,owner)||(owner&~std::uint64_t(1))!=field)continue;
                auto candidate=child(offset,"inner");
                if(candidate!="null"){inner=std::move(candidate);result+=",\"inner_offset\":"+std::to_string(offset);break;}
            }
            result+=",\"inner\":"+inner;
        }
        if(type=="SetProperty"||type=="OptionalProperty")result+=",\"inner\":"+child(extra,"inner");
        if(type=="MapProperty")result+=",\"key\":"+child(extra,"key")+",\"value\":"+child(extra+8,"value");
        if(type=="EnumProperty")result+=",\"underlying\":"+child(extra,"underlying");
        return result+"}";
    }
    bool collect(bool includeNetwork=true) {
        if(!layoutVerified_&&!discover())return false;
        checkDeadline();
        if(!arrayHeader(objects_,chunkTable_,count_)){errors.push_back("object array changed before collection");return false;}
        objects.clear();types.clear();objectMap_.clear();optionalUnresolved.clear();invalidObjectSamples.clear();fieldLayoutProbes.clear();
        names_.clear();
        nullSlots=invalidObjects=propertyCount=rpcCount=replicatedPropertyCount=0;propertyLayoutVerified_=false;
        for(std::int32_t i=0;i<count_;++i){if((i&255)==0){checkDeadline();report("objects",i,count_);}auto address=objectAt(chunkTable_,i);if(!address){++nullSlots;continue;}Object o;o.address=address;o.index=i;std::int32_t actual=-1;
            if(!objectBasic(address)||!read(address,l_.objIndex,actual)||actual!=i||!read(address,l_.objClass,o.klass)){
                if(invalidObjectSamples.size()<20){read(address,l_.objIndex,actual);invalidObjectSamples.push_back("slot="+std::to_string(i)+" address="+hex(address)+" index="+std::to_string(actual)+" name="+name(address,l_.objName)+nameDiagnostic(address));}
                ++invalidObjects;continue;
            }o.name=name(address,l_.objName);o.className=name(o.klass,l_.objName);o.path=path(address);if(!plausible(o.path)||!plausible(o.className)){
                if(invalidObjectSamples.size()<20)invalidObjectSamples.push_back("slot="+std::to_string(i)+" address="+hex(address)+" name="+o.name+" class="+o.className+" invalid path/class"+nameDiagnostic(address));
                ++invalidObjects;continue;
            }objects.push_back(o);objectMap_[address]=o;}
        if(objects.size()<3){errors.push_back("fewer than three valid objects collected");return false;}
        inferPropertyLayout();
        runtimeVtables.clear();
        for(const auto& object:objects)if(object.path=="/Script/CoreUObject.Default__Object"){
            std::uint64_t vtable=0;
            if(mem_.get(object.address,vtable)){
                std::string record="{\"object\":"+quote(object.path)+",\"entries\":[";
                for(unsigned i=0;i<96;++i){std::uint64_t fn=0;mem_.get(vtable+i*8,fn);if(i)record+=',';record+=(fn>=module_.base&&fn<module_.base+module_.size)?quote(hex(fn-module_.base)):"null";}
                runtimeVtables.push_back(record+"]}");
            }
        }
        std::size_t processed=0;
        for(const auto& o:objects){if((processed&127)==0){checkDeadline();report("types",processed,objects.size());}++processed;Type t;t.address=o.address;t.name=o.name;t.path=o.path;
            if(o.className=="Class"||o.className=="BlueprintGeneratedClass"||(o.className.size()>14&&o.className.substr(o.className.size()-14)=="GeneratedClass"))t.kind="class";
            else if(o.className=="ScriptStruct")t.kind="struct";else if(o.className=="Function"||o.className=="DelegateFunction"||o.className=="SparseDelegateFunction")t.kind="function";else if(o.className=="Enum"||o.className=="UserDefinedEnum")t.kind="enum";else continue;
            if(t.kind!="enum"){std::int32_t size=0;std::uint64_t super=0;if(read(t.address,l_.structSize,size)&&size>=0&&size<=l_.maxTypeSize)t.size=size;else {t.complete=false;errors.push_back("invalid UStruct size: "+t.path);}
                std::uint32_t alignment=0;if(read(t.address,l_.structSize+4,alignment)&&alignment&&alignment<=64&&(alignment&(alignment-1))==0)t.alignment=alignment;
                if(read(t.address,l_.structSuper,super)){if(super&&objectMap_.count(super))t.super=objectMap_.at(super).path;else if(super){t.complete=false;errors.push_back("super is absent from object inventory: "+t.path);}}else{t.complete=false;errors.push_back("unreadable SuperStruct: "+t.path);}
                if(t.kind=="function"){std::uint32_t flags=0;std::uint16_t ps=0;std::uint64_t fn=0;if(read(t.address,l_.functionFlags,flags))t.flags=flags;if(read(t.address,l_.functionParmsSize,ps)&&(!t.size||ps<=*t.size))t.parameterSize=ps;else{t.complete=false;errors.push_back("unreadable function parameter size: "+t.path);}if(read(t.address,l_.functionNative,fn)&&fn>=module_.base&&fn<module_.base+module_.size&&mem_.executable(fn))t.nativeRva=fn-module_.base;
                    if(flags&0x40){++rpcCount;std::uint16_t id=0;if(l_.functionRpcId>=0&&read(t.address,l_.functionRpcId,id))t.rpcId=id;if(l_.functionRpcResponseId>=0&&read(t.address,l_.functionRpcResponseId,id))t.rpcResponseId=id;}
                }
                readFields(t);
            }else {std::uint64_t data=0;std::int32_t n=0,capacity=0;
                if(!read(t.address,l_.enumNames,data)||!read(t.address,l_.enumNames+8,n)||!read(t.address,l_.enumNames+12,capacity)||n<0||n>65536||capacity<n||capacity>1000000||(n&&!ptr(data))){t.complete=false;errors.push_back("invalid enum array: "+t.path);}
                else for(int i=0;i<n;++i){auto at=data+(std::uint64_t)i*l_.enumStride;auto en=name(at,0);std::int64_t v=0;if(!plausible(en)||!mem_.get(at+8,v)){t.complete=false;errors.push_back("invalid enum item: "+t.path);break;}t.enumValues.push_back({en,v});}}
            types.push_back(std::move(t));}
        if(types.empty()){errors.push_back("no reflected types found; class/layout validation is insufficient");return false;}
        if(invalidObjects)unresolved.push_back(std::to_string(invalidObjects)+" non-null object slots failed validation (mutation or unknown layout)");
        if(includeNetwork&&config_.number("collect_network",1)!=0){
            collectClassNetworkTables();
            collectRuntimeNetworkCaches();
        }
        propertyLayoutVerified_=propertyCount>0;
        if(!propertyCount)unresolved.push_back("no FProperty metadata was recovered");
        std::sort(optionalUnresolved.begin(),optionalUnresolved.end());optionalUnresolved.erase(std::unique(optionalUnresolved.begin(),optionalUnresolved.end()),optionalUnresolved.end());
        return errors.empty()&&unresolved.empty();
    }
    std::string propertyJson(const Property& p)const {
        return "{\"kind\":\"property\",\"name\":"+quote(p.name)+",\"address\":"+quote(hex(p.address))+",\"type\":"+quote(p.type)+",\"offset\":"+std::to_string(p.offset)+",\"element_size\":"+std::to_string(p.element)+",\"array_dim\":"+std::to_string(p.array)+",\"size\":"+std::to_string((std::int64_t)p.array*p.element)+",\"flags\":"+quote(hex(p.flags))+",\"target\":"+(p.target.empty()?"null":quote(p.target))+",\"role\":"+quote(p.role)+",\"bool_mask\":"+(p.boolMask?std::to_string(*p.boolMask):"null")+",\"rep_index\":"+(p.repIndex?std::to_string(*p.repIndex):"null")+",\"rep_condition\":"+(p.repCondition?std::to_string(*p.repCondition):"null")+",\"rep_notify\":"+(p.repNotify.empty()?"null":quote(p.repNotify))+",\"detail\":"+p.detail+"}";
    }
    void collectClassNetworkTables() {
        classNetworkTables.clear();
        std::map<std::uint64_t,const Type*> functions;
        std::map<std::uint64_t,std::pair<const Type*,const Property*>> properties;
        std::set<std::string> owners;
        std::uint64_t classSize=512;
        for(const auto& t:types){
            if(t.path=="/Script/CoreUObject.Class"&&t.size)classSize=std::min<std::uint64_t>(*t.size,4096);
            if(t.kind=="function"&&t.flags&&(*t.flags&0x40)){functions[t.address]=&t;owners.insert(t.path.substr(0,t.path.rfind('.')));}
            if(t.kind=="class")for(const auto& p:t.fields)if(p.flags&0x20){properties[p.address]={&t,&p};owners.insert(t.path);}
        }
        std::map<std::string,const Type*> byPath;for(const auto& t:types)if(t.kind=="class")byPath[t.path]=&t;
        for(const auto& t:types){
            if(t.kind!="class"||!owners.count(t.path))continue;
            checkDeadline();
            std::set<std::string> lineage;
            const Type* current=&t;
            while(current&&lineage.size()<64&&lineage.insert(current->path).second){auto it=byPath.find(current->super);current=it==byPath.end()?nullptr:it->second;}
            for(std::uint64_t offset=0xb0;offset+16<=classSize;offset+=8){
                std::uint64_t data=0;std::int32_t n=0,capacity=0;
                if(!read(t.address,offset,data)||!read(t.address,offset+8,n)||!read(t.address,offset+12,capacity)||
                   n<1||n>8192||capacity<n||capacity>65536||!ptr(data))continue;
                std::string rpcRows,repRows;bool validRpc=true,validRep=true;
                for(int i=0;i<n&&(validRpc||validRep);++i){
                    if((i&127)==0)checkDeadline();
                    if(validRpc){
                        std::uint64_t address=0;mem_.get(data+(std::uint64_t)i*8,address);
                        auto f=functions.find(address);
                        if(f==functions.end()||!lineage.count(f->second->path.substr(0,f->second->path.rfind('.'))))validRpc=false;
                        else{if(i)rpcRows+=',';rpcRows+="{\"position\":"+std::to_string(i)+",\"path\":"+quote(f->second->path)+"}";}
                    }
                    if(validRep){
                        std::uint64_t address=0;std::int32_t arrayIndex=-1;
                        mem_.get(data+(std::uint64_t)i*16,address);mem_.get(data+(std::uint64_t)i*16+8,arrayIndex);
                        auto p=properties.find(address);
                        if(p==properties.end()||!lineage.count(p->second.first->path)||arrayIndex<0||arrayIndex>=p->second.second->array||
                           !p->second.second->repIndex||*p->second.second->repIndex+arrayIndex!=i)validRep=false;
                        else{if(i)repRows+=',';repRows+="{\"rep_index\":"+std::to_string(i)+",\"owner\":"+quote(p->second.first->path)+",\"property\":"+quote(p->second.second->name)+",\"array_index\":"+std::to_string(arrayIndex)+"}";}
                    }
                }
                if(validRpc||validRep)classNetworkTables.push_back("{\"class\":"+quote(t.path)+",\"kind\":"+quote(validRpc?"class_net_fields":"class_reps")+",\"header_offset\":"+std::to_string(offset)+",\"entries\":["+(validRpc?rpcRows:repRows)+"]}");
            }
        }
    }
    std::string systematicNetworkJson()const {
        const std::string target="/Script/HTGame.SystematicGameFeaturesComponent";
        std::map<std::string,const Type*> classes;
        for(const auto& type:types)if(type.kind=="class")classes[type.path]=&type;
        std::set<std::string> hierarchy;auto path=target;
        while(!path.empty()&&hierarchy.size()<64&&hierarchy.insert(path).second){
            const auto it=classes.find(path);if(it==classes.end())break;path=it->second->super;
        }
        bool first=true,targetObserved=false;
        std::string result="{\"target_class\":"+quote(target)+",\"cache_chain\":[";
        for(const auto& cache:runtimeNetwork.cacheRecords)if(hierarchy.count(cache.classPath)){
            if(!first)result+=',';first=false;result+=cache.json;
            if(cache.classPath==target)targetObserved=true;
        }
        result+="],\"target_functions\":{";
        const std::array<const char*,3> requested={"ClientGameFeaturesToReadyForActivate","ClientOnSystematicGameFeatureActivated","ServerSetupSystematicGameFeatures"};
        bool complete=targetObserved;
        for(const auto& ancestor:hierarchy){
            bool observed=false;for(const auto& cache:runtimeNetwork.cacheRecords)if(cache.classPath==ancestor)observed=true;
            complete=complete&&observed;
        }
        for(std::size_t i=0;i<requested.size();++i){
            if(i)result+=',';result+=quote(requested[i])+":{\"observations\":[";
            bool observed=false;
            for(const auto& cache:runtimeNetwork.cacheRecords)if(cache.classPath==target)
                for(const auto& entry:cache.entries)if(entry.path==target+"."+requested[i]){
                    if(observed)result+=',';observed=true;
                    result+="{\"field_net_index\":"+std::to_string(entry.fieldNetIndex)+
                        ",\"cache_address\":"+quote(hex(cache.address))+",\"incompatible\":"+(entry.incompatible?"true":"false")+"}";
                    if(entry.incompatible)complete=false;
                }
            complete=complete&&observed;
            result+="],\"status\":"+quote(observed?"runtime_observed":"not_observed")+"}";
        }
        return result+"},\"status\":"+quote(complete?"runtime_observed":targetObserved?"partial":"not_observed")+
            ",\"packet_verified\":false}";
    }
    std::string networkJson()const {
        std::string out="{\"format\":\"EbonNetwork-v1\",\"rpc_count\":"+std::to_string(rpcCount)+",\"replicated_property_count\":"+std::to_string(replicatedPropertyCount)+",\"rpcs\":[";
        bool first=true;
        for(const auto& t:types)if(t.kind=="function"&&t.flags&&(*t.flags&0x40)){
            if(!first)out+=',';first=false;
            const auto f=*t.flags;
            const auto direction=(f&0x200000)?"server":((f&0x1000000)?"client":((f&0x4000)?"multicast":"unspecified"));
            out+="{\"path\":"+quote(t.path)+",\"flags\":"+quote(hex(f))+",\"direction\":"+quote(direction)+",\"reliable\":"+((f&0x80)?"true":"false")+",\"rpc_id\":"+(t.rpcId?std::to_string(*t.rpcId):"null")+",\"rpc_response_id\":"+(t.rpcResponseId?std::to_string(*t.rpcResponseId):"null")+",\"field_net_index\":null}";
        }
        out+="],\"properties\":[";first=true;
        for(const auto& t:types)if(t.kind=="class")for(const auto& p:t.fields)if(p.flags&0x20){
            if(!first)out+=',';first=false;
            out+="{\"owner\":"+quote(t.path)+",\"property\":"+quote(p.name)+",\"flags\":"+quote(hex(p.flags))+",\"rep_index\":"+(p.repIndex?std::to_string(*p.repIndex):"null")+",\"rep_condition\":"+(p.repCondition?std::to_string(*p.repCondition):"null")+",\"rep_notify\":"+(p.repNotify.empty()?"null":quote(p.repNotify))+",\"rep_layout_handle\":null}";
        }
        out+="],\"class_tables\":[";for(std::size_t i=0;i<classNetworkTables.size();++i){if(i)out+=',';out+=classNetworkTables[i];}
        out+="],\"active_network_objects\":[";first=true;
        for(const auto& o:objects){
            if(o.name.compare(0,9,"Default__")==0)continue;
            if(o.className.find("NetDriver")==std::string::npos&&o.className.find("NetConnection")==std::string::npos&&o.className.find("ReplicationBridge")==std::string::npos)continue;
            if(!first)out+=',';first=false;out+="{\"path\":"+quote(o.path)+",\"class\":"+quote(o.className)+",\"address\":"+quote(hex(o.address))+"}";
        }
        out+="],\"layout_offsets\":{\"function_rpc_id\":"+std::to_string(l_.functionRpcId)+",\"function_rpc_response_id\":"+std::to_string(l_.functionRpcResponseId)+",\"property_rep_index\":"+std::to_string(l_.propertyRepIndex)+",\"property_rep_condition\":"+std::to_string(l_.propertyRepCondition)+",\"property_rep_notify\":"+std::to_string(l_.propertyRepNotify)+"}";
        out+=",\"runtime_cache_candidates\":[";for(std::size_t i=0;i<networkCacheCandidates.size();++i){if(i)out+=',';out+=networkCacheCandidates[i];}out+="]";
        out+=",\"runtime_cache_records\":[";for(std::size_t i=0;i<runtimeNetwork.cacheRecords.size();++i){if(i)out+=',';out+=runtimeNetwork.cacheRecords[i].json;}out+="]";
        out+=",\"systematic_game_features_component\":"+systematicNetworkJson();
        out+=",\"rep_layouts\":[";for(std::size_t i=0;i<runtimeNetwork.layouts.size();++i){if(i)out+=',';out+=runtimeNetwork.layouts[i];}out+="]";
        std::string reasons;for(const auto& reason:runtimeNetwork.budgetReasons){if(!reasons.empty())reasons+=',';reasons+=quote(reason);}
        out+=",\"runtime_scan\":{\"roots\":"+std::to_string(runtimeNetwork.roots)+",\"nodes\":"+std::to_string(runtimeNetwork.nodes)+",\"bytes\":"+std::to_string(runtimeNetwork.bytes)+",\"traversal_bytes\":"+std::to_string(runtimeNetwork.traversalBytes)+",\"layout_probes\":"+std::to_string(runtimeNetwork.layoutProbes)+",\"graph_retained\":"+(runtimeNetwork.graphRetained?"true":"false")+",\"read_failures\":"+std::to_string(runtimeNetwork.readFailures)+",\"incomplete_records\":"+std::to_string(runtimeNetwork.incompleteRecords)+",\"budget_reasons\":["+reasons+"],\"budget_exhausted\":"+(runtimeNetwork.budgetExhausted?"true":"false")+"}";
        return out+",\"runtime_mapping_status\":"+quote(runtimeNetwork.cacheRecords.empty()?"not_observed":"runtime_cache_observed")+",\"limitations\":[\"RPCId is UFunction service metadata, not an assumed wire FieldNetIndex\",\"RepIndex and class table positions are not FRepLayout handles\",\"FClassNetCache, FRepLayout and Iris handle mappings require an observed active network context\"]}\n";
    }
    void collectRuntimeNetworkCaches() {
        std::map<std::uint64_t,NetworkRuntimeClass> classes;
        std::map<std::uint64_t,NetworkRuntimeField> fields;
        std::set<std::uint64_t> allObjects;
        std::vector<std::uint64_t> roots;
        std::map<std::uint64_t,unsigned> rootSizes;
        std::set<std::uint64_t> rootSet;
        for(const auto& o:objects){
            allObjects.insert(o.address);
        }
        std::map<std::string,const Type*> classesByPath;
        for(const auto& t:types){
            if(!t.complete)continue;
            if(t.kind=="class"){
                const auto it=objectMap_.find(t.address);
                if(it!=objectMap_.end()){
                    classes[t.address]={t.address,it->second.index,t.path,t.super};
                    classesByPath[t.path]=&t;
                }
            }
            if(t.kind=="function"&&t.flags&&(*t.flags&0x40))fields[t.address]={t.address,t.path,t.path.substr(0,t.path.rfind('.')),0,true,1};
            for(const auto& p:t.fields){
                fields[p.address]={p.address,t.path+"."+p.name,t.path,p.offset,false,p.element,p.array};
                for(const auto& inner:p.innerFields)fields[inner.address]=inner;
            }
        }
        const auto addRoot=[&](const Object& object,const NetworkRuntimeClass* cls){
            if(!rootSet.insert(object.address).second)return;
            roots.push_back(object.address);
            unsigned size=4096;
            if(cls){const auto type=classesByPath.find(cls->path);if(type!=classesByPath.end()&&type->second->size)
                size=(unsigned)std::min<std::int32_t>(std::max<std::int32_t>(*type->second->size,1),65536);}
            rootSizes[object.address]=size;
        };
        // Queue the active network driver first. UObject pointers are excluded
        // from generic graph expansion below, so important replication owners
        // must also be explicit roots rather than merely reachable UObjects.
        for(const auto& o:objects){
            if(o.name.compare(0,9,"Default__")==0||o.className.find("NetDriver")==std::string::npos)continue;
            const auto cls=classes.find(o.klass);addRoot(o,cls==classes.end()?nullptr:&cls->second);
        }
        for(const auto& o:objects){
            if(o.name.rfind("Default__",0)==0)continue;
            auto cls=classes.find(o.klass);std::set<std::string> lineage;bool target=false;
            for(auto ancestor=cls;ancestor!=classes.end()&&lineage.insert(ancestor->second.path).second;){
                if(ancestor->second.path=="/Script/HTGame.SystematicGameFeaturesComponent"){target=true;break;}
                auto parent=classesByPath.find(ancestor->second.super);
                if(parent==classesByPath.end())break;ancestor=classes.find(parent->second->address);
            }
            if(!target)continue;
            std::uint32_t flags=0;
            if(read(o.address,l_.objFlags,flags)&&(flags&0x30u))continue;
            addRoot(o,cls==classes.end()?nullptr:&cls->second);
            const Object* owner=&o;std::set<std::uint64_t> seen;
            for(unsigned n=0;n<16;++n){
                std::uint64_t outer=0;if(!read(owner->address,l_.objOuter,outer)||!seen.insert(outer).second)break;
                const auto next=objectMap_.find(outer);if(next==objectMap_.end())break;owner=&next->second;
                auto ownerClass=classes.find(owner->klass);
                addRoot(*owner,ownerClass==classes.end()?nullptr:&ownerClass->second);
            }
        }
        const std::set<std::string> focusPaths={
            "/Script/Engine.ActorChannel",
            "/Script/Engine.NetConnection",
            "/Script/Engine.ReplicationBridge",
            "/Script/Engine.PlayerState",
            "/Script/Engine.GameStateBase",
            "/Script/Engine.WorldDataLayers",
            "/Script/HTGame.HTPlayerCharacter",
            "/Script/HTGame.SystematicGameFeaturesComponent",
            "/Script/HTGame.HTSystematicComponent",
            "/Script/HTGame.HTPlayerController",
            "/Script/Engine.CharacterMovementComponent",
            "/Script/HTGame.HTPlayerCharacterMovementComponent",
            "/Script/HTGame.HTAbilitySystemComponent",
            "/Script/HTGame.HTAttributeComponent",
            "/Script/HTGame.HTAbilityComponent",
            "/Script/HTGame.HTSkillEffectComponent",
            "/Script/HTGame.HTCharacterStateManagerComponent",
            "/Script/GameplayAbilities.AbilitySystemComponent",
            "/Script/HTGame.GameplayAttributeSet",
            "/Script/HTGame.HTPlayerAttributeSet",
            "/Script/HTGame.HTCharacterAttributeSet"
        };
        const auto focusedClass=[&](std::uint64_t address){
            std::set<std::string> seen;auto current=classes.find(address);
            while(current!=classes.end()&&seen.size()<64&&seen.insert(current->second.path).second){
                if(focusPaths.count(current->second.path))return true;
                const auto parent=classesByPath.find(current->second.super);
                if(parent==classesByPath.end())break;
                current=classes.find(parent->second->address);
            }
            return false;
        };
        const auto activeCandidate=[](const Object& o){
            return o.path.rfind("/Engine/Transient.",0)==0||o.path.find(".PersistentLevel.")!=std::string::npos||
                   o.path.rfind("/Temp/",0)==0;
        };
        std::set<std::uint64_t> focusedClassRoots;
        for(const auto& o:objects){
            if(o.name.compare(0,9,"Default__")==0||rootSet.count(o.address)||!activeCandidate(o))continue;
            const auto cls=classes.find(o.klass);
            if(cls==classes.end()||!focusedClass(o.klass))continue;
            // One live instance is enough to reach a class's shared layout;
            // retain every ActorChannel because each owns a different actor
            // replicator and can expose a distinct target layout.
            if(cls->second.path!="/Script/Engine.ActorChannel"&&!focusedClassRoots.insert(o.klass).second)continue;
            addRoot(o,&cls->second);
        }
        std::vector<std::string> replicatedActorPaths;
        const auto actorChannelType=classesByPath.find("/Script/Engine.ActorChannel");
        const Property* channelActorProperty=nullptr;
        if(actorChannelType!=classesByPath.end())for(const auto& property:actorChannelType->second->fields)
            if(property.name=="Actor"&&property.type=="ObjectProperty"){channelActorProperty=&property;break;}
        if(channelActorProperty)for(const auto& o:objects){
            if(!rootSet.count(o.address)||o.name.compare(0,9,"Default__")==0)continue;
            const auto cls=classes.find(o.klass);
            if(cls==classes.end()||cls->second.path!="/Script/Engine.ActorChannel")continue;
            std::uint64_t actorAddress=0;
            if(!read(o.address,channelActorProperty->offset,actorAddress)||!objectMap_.count(actorAddress))continue;
            const auto& actor=objectMap_.at(actorAddress);
            const auto actorClass=classes.find(actor.klass);
            if(actorClass==classes.end())continue;
            addRoot(actor,&actorClass->second);
            replicatedActorPaths.push_back(actor.path);
        }
        for(const auto& o:objects){
            if(o.name.compare(0,9,"Default__")==0||rootSet.count(o.address)||!activeCandidate(o))continue;
            bool belongsToReplicatedActor=false;
            for(const auto& actorPath:replicatedActorPaths){
                if(o.path.size()>actorPath.size()&&o.path.compare(0,actorPath.size(),actorPath)==0&&o.path[actorPath.size()]=='.'){
                    belongsToReplicatedActor=true;break;
                }
            }
            if(!belongsToReplicatedActor)continue;
            const auto cls=classes.find(o.klass);
            if(cls!=classes.end()&&focusedClass(o.klass))addRoot(o,&cls->second);
        }
        const std::function<void()> check=[&](){checkDeadline();};
        NetworkRuntimeDecoder decoder(mem_,classes,fields,check);
        report("network_runtime",0,roots.size());
        const auto scanMs=config_.number("network_scan_budget_ms",20000);
        if(scanMs<1000||scanMs>120000)throw std::runtime_error("network_scan_budget_ms must be in 1000..120000");
        const auto bounded=[&](const char* key,std::int64_t fallback,std::int64_t low,std::int64_t high){
            const auto n=config_.number(key,fallback);
            if(n<low||n>high)throw std::runtime_error(std::string(key)+" outside supported bounds");return n;
        };
        NetworkScanLimits limits;
        limits.maxNodes=(std::size_t)bounded("network_max_nodes",32768,1,262144);
        limits.maxQueuedNodes=(std::size_t)bounded("network_max_queue",65536,1,524288);
        limits.maxBytes=(std::size_t)bounded("network_evidence_mb",16,1,64)*1024*1024;
        limits.maxTraversalBytes=(std::size_t)bounded("network_traversal_mb",64,1,512)*1024*1024;
        limits.maxDepth=(unsigned)bounded("network_max_depth",4,1,8);
        limits.retainGraph=bounded("network_capture_graph",1,0,1)!=0;
        runtimeNetwork=decoder.scan(roots,module_,(unsigned)scanMs,allObjects,rootSizes,limits);
        networkCacheCandidates=runtimeNetwork.caches;
        report("network_runtime",runtimeNetwork.nodes,runtimeNetwork.nodes);
    }
    std::string networkEvidenceJson()const {
        const auto bytes=[&](std::uint64_t address,std::size_t count){
            std::vector<unsigned char> data(count);
            if(!mem_.copy(address,data.data(),count))return std::string("null");
            static const char digits[]="0123456789abcdef";std::string encoded;encoded.reserve(count*2);
            for(auto c:data){encoded+=digits[c>>4];encoded+=digits[c&15];}return quote(encoded);
        };
        std::string out="{\"format\":\"EbonNetworkEvidence-v1\",\"properties\":[";bool first=true;
        std::set<std::string> owners;
        for(const auto& t:types)if(t.kind=="class")for(const auto& p:t.fields)if(p.flags&0x20){
            if(!first)out+=',';first=false;owners.insert(t.path);
            out+="{\"owner\":"+quote(t.path)+",\"name\":"+quote(p.name)+",\"address\":"+quote(hex(p.address))+",\"bytes\":"+bytes(p.address,112)+"}";
        }
        out+="],\"functions\":[";first=true;
        for(const auto& t:types)if(t.kind=="function"&&t.flags&&(*t.flags&0x40)){
            if(!first)out+=',';first=false;owners.insert(t.path.substr(0,t.path.rfind('.')));
            out+="{\"path\":"+quote(t.path)+",\"bytes\":"+bytes(t.address+176,48)+",\"start_offset\":176}";
        }
        out+="],\"classes\":[";first=true;
        for(const auto& t:types)if(t.kind=="class"&&owners.count(t.path)){
            if(!first)out+=',';first=false;
            out+="{\"path\":"+quote(t.path)+",\"address\":"+quote(hex(t.address))+",\"bytes\":"+bytes(t.address,512)+"}";
        }
        return out+"]}\n";
    }
    void write(const std::filesystem::path& out,bool ok)const {
        std::filesystem::create_directories(out);save(out/"dump_state.json","{\"status\":\"writing\",\"success\":false}\n");
        std::ostringstream os,ts;
        for(const auto& o:objects){
            std::uint64_t outer=0;std::uint32_t flags=0;
            read(o.address,l_.objOuter,outer);const bool flagsRead=read(o.address,l_.objFlags,flags);
            const auto parent=objectMap_.find(outer),klass=objectMap_.find(o.klass);
            os<<"{\"index\":"<<o.index<<",\"address\":"<<quote(hex(o.address))<<",\"path\":"<<quote(o.path)
              <<",\"name\":"<<quote(o.name)<<",\"class\":"<<quote(o.className)
              <<",\"class_path\":"<<(klass==objectMap_.end()?"null":quote(klass->second.path))
              <<",\"outer\":"<<(parent==objectMap_.end()?"null":quote(parent->second.path))
              <<",\"object_flags\":"<<(flagsRead?quote(hex(flags)):"null")
              <<",\"is_default_subobject\":"<<(flagsRead?((flags&0x00040000u)?"true":"false"):"null")<<"}\n";
        }
        for(const auto& t:types){ts<<"{\"kind\":"<<quote(t.kind)<<",\"name\":"<<quote(t.name)<<",\"path\":"<<quote(t.path)<<",\"address\":"<<quote(hex(t.address))<<",\"size\":"<<(t.size?std::to_string(*t.size):"null")<<",\"alignment\":"<<(t.alignment?std::to_string(*t.alignment):"null")<<",\"parameter_size\":"<<(t.parameterSize?std::to_string(*t.parameterSize):"null")<<",\"super\":"<<(t.super.empty()?"null":quote(t.super))<<",\"flags\":"<<(t.flags?quote(hex(*t.flags)):"null")<<",\"rpc_id\":"<<(t.rpcId?std::to_string(*t.rpcId):"null")<<",\"rpc_response_id\":"<<(t.rpcResponseId?std::to_string(*t.rpcResponseId):"null")<<",\"native_rva\":"<<(t.nativeRva?quote(hex(*t.nativeRva)):"null")<<",\"complete\":"<<(t.complete?"true":"false")<<",\"fields\":[";for(std::size_t i=0;i<t.fields.size();++i){if(i)ts<<',';ts<<propertyJson(t.fields[i]);}ts<<"],\"enum_values\":[";for(std::size_t i=0;i<t.enumValues.size();++i){if(i)ts<<',';ts<<"{\"name\":"<<quote(t.enumValues[i].first)<<",\"value\":"<<t.enumValues[i].second<<'}';}ts<<"]}\n";}
        save(out/"objects.jsonl",os.str());save(out/"types.jsonl",ts.str());
        writeNetwork(out);
        writeState(out,ok);
    }
    void writeNetwork(const std::filesystem::path& out)const {
        save(out/"network_metadata.json",networkJson());
        save(out/"network_evidence.json",networkEvidenceJson());
        std::string blockIndex="{\"format\":\"EbonNetworkMemory-v1\",\"blocks\":[",memoryBytes;
        for(std::size_t i=0;i<runtimeNetwork.blocks.size();++i){
            const auto& block=runtimeNetwork.blocks[i];if(i)blockIndex+=',';
            blockIndex+="{\"address\":"+quote(hex(block.first))+",\"offset\":"+std::to_string(memoryBytes.size())+",\"size\":"+std::to_string(block.second.size())+"}";
            memoryBytes.append(reinterpret_cast<const char*>(block.second.data()),block.second.size());
        }
        save(out/"network_memory.bin",memoryBytes);save(out/"network_memory.json",blockIndex+"]}\n");

    }
    // Progress/failure states do not create empty metadata files or claim a successful dump.
    void writeState(const std::filesystem::path& out,bool ok,const std::string& status="")const {
        std::filesystem::create_directories(out);
        auto array=[](const auto& list){std::string s="[";for(std::size_t i=0;i<list.size();++i){if(i)s+=',';s+=quote(list[i]);}return s+"]";};
        std::vector<std::string> pools,arrays;for(auto p:poolCandidates)pools.push_back(hex(p-module_.base));for(auto p:arrayCandidates)arrays.push_back(hex(p-module_.base));
        std::ostringstream state;state<<"{\n  \"format\":\"EbonReflection-v1\",\n  \"status\":"<<quote(!status.empty()?status:(!ok?"failed":(optionalUnresolved.empty()?"complete":"complete_with_unresolved")))<<",\n  \"success\":"<<(ok?"true":"false")<<",\n  \"layout_verified\":"<<(layoutVerified_?"true":"false")<<",\n  \"property_layout_verified\":"<<(propertyLayoutVerified_?"true":"false")<<",\n  \"build\":{\"module\":"<<quote(utf8(module_.name))<<",\"sha256\":"<<(moduleSha256.empty()?"null":quote(moduleSha256))<<"},\n  \"module_base\":"<<quote(hex(module_.base))<<",\n  \"name_pool_rva\":"<<(pool_?quote(hex(pool_-module_.base)):"null")<<",\n  \"object_array_rva\":"<<(objects_?quote(hex(objects_-module_.base)):"null")<<",\n  \"objects\":"<<objects.size()<<",\n  \"types\":"<<types.size()<<",\n  \"properties\":"<<propertyCount<<",\n  \"null_slots\":"<<nullSlots<<",\n  \"invalid_objects\":"<<invalidObjects<<",\n  \"errors\":"<<array(errors)<<",\n  \"unresolved\":"<<array(unresolved)<<",\n  \"optional_unresolved\":"<<array(optionalUnresolved)<<",\n  \"name_pool_candidates\":"<<array(pools)<<",\n  \"object_array_candidates\":"<<array(arrays)<<",\n  \"coverage\":\"loaded reflected objects only; asset and protocol coverage not established\"\n}\n";
        auto stateText=state.str();
        std::ostringstream offsets;
        offsets<<"  \"runtime_vtables\": [";
        for(std::size_t i=0;i<runtimeVtables.size();++i){if(i)offsets<<',';offsets<<runtimeVtables[i];}
        offsets<<"],\n";
        offsets<<"  \"invalid_object_samples\": "<<array(invalidObjectSamples)<<",\n"
               <<"  \"field_layout_probes\": "<<array(fieldLayoutProbes)<<",\n";
        offsets<<"  \"property_layout_inferred\": "<<(propertyLayoutInferred_?"true":"false")<<",\n"
               <<"  \"field_layout\": {\"class\":"<<l_.fieldClass<<",\"next\":"<<l_.fieldNext<<",\"name\":"<<l_.fieldName
               <<",\"array_dim\":"<<l_.propArray<<",\"element_size\":"<<l_.propElement<<",\"flags\":"<<l_.propFlags<<",\"offset\":"<<l_.propOffset<<"},\n";
        stateText.insert(stateText.find('\n')+1,offsets.str());
        stateText.insert(stateText.find('\n')+1,"  \"phase\": "+quote(progressPhase)+",\n  \"progress_done\": "+std::to_string(progressDone)+",\n  \"progress_total\": "+std::to_string(progressTotal)+",\n");
        save(out/"dump_state.json.tmp",stateText);
        // Windows readers may omit FILE_SHARE_DELETE. Their short-lived read
        // handle must not abort a capture which is publishing a checkpoint.
        for(unsigned attempt=0;;++attempt){
            if(MoveFileExW((out/"dump_state.json.tmp").c_str(),(out/"dump_state.json").c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))break;
            const auto error=GetLastError();
            if(attempt>=49||(error!=ERROR_SHARING_VIOLATION&&error!=ERROR_ACCESS_DENIED))
                throw std::runtime_error("cannot commit dump state, Win32="+std::to_string(error));
            Sleep(10);
        }
    }
};
} // namespace ebon
