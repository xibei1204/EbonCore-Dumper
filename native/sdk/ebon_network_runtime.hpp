#pragma once
#include "../common/ebon_common.hpp"

namespace ebon {
struct NetworkRuntimeClass {std::uint64_t address=0;std::int32_t index=-1;std::string path,super;};
struct NetworkRuntimeField {std::uint64_t address=0;std::string path,owner;std::int32_t offset=0;bool rpc=false;std::int32_t elementSize=1,arrayDim=1;};
struct NetworkRuntimeCacheEntry {std::string path;std::int32_t fieldNetIndex=-1;std::uint32_t checksum=0;bool incompatible=false;};
struct NetworkRuntimeCacheRecord {
    std::string classPath,superClassPath,json;
    std::uint64_t address=0,superAddress=0;
    std::int32_t fieldsBase=0,fieldCount=0,fieldIndexEndExclusive=0;
    unsigned fieldStride=0;
    std::uint32_t classChecksum=0;
    std::vector<NetworkRuntimeCacheEntry> entries;
};
struct NetworkLayoutStorage {
    std::uint64_t parents=0,commands=0;
    unsigned parentCount=0,commandCount=0,parentStride=0,commandStride=0;
};
struct NetworkRuntimeResult {
    std::vector<std::string> caches,layouts;
    std::vector<NetworkRuntimeCacheRecord> cacheRecords;
    std::vector<std::pair<std::uint64_t,std::vector<std::uint8_t>>> blocks;
    std::size_t nodes=0,roots=0,bytes=0,readFailures=0,incompleteRecords=0;
    bool budgetExhausted=false;std::set<std::string> budgetReasons;
};
class NetworkRuntimeDecoder {
    const MemoryReader& memory_;
    const std::map<std::uint64_t,NetworkRuntimeClass>& classes_;
    const std::map<std::uint64_t,NetworkRuntimeField>& fields_;
    std::map<std::int32_t,const NetworkRuntimeClass*> classIndices_;
    const std::function<void()>& check_;
    template<class T> bool get(std::uint64_t a,T& v)const{return memory_.get(a,v);}
    template<class T> T value(std::uint64_t a)const{T v{};get(a,v);return v;}
    const NetworkRuntimeField* field(std::uint64_t a)const {auto it=fields_.find(a&~std::uint64_t(1));return it==fields_.end()?nullptr:&it->second;}
    bool belongs(const std::string& owner,const NetworkRuntimeClass& cls)const {
        auto path=cls.path;std::set<std::string> seen;
        while(!path.empty()&&seen.size()<64&&seen.insert(path).second){
            if(path==owner)return true;
            auto it=std::find_if(classes_.begin(),classes_.end(),[&](const auto& pair){return pair.second.path==path;});
            if(it==classes_.end())break;path=it->second.super;
        }
        return false;
    }
    bool array(std::uint64_t header,std::uint64_t& data,std::int32_t& count)const {
        const auto capacity=value<std::int32_t>(header+12);
        return get(header,data)&&get(header+8,count)&&data>=0x10000&&!(data&7)&&count>0&&count<=8192&&capacity>=count&&capacity<=65536;
    }
    bool cacheArray(std::uint64_t header,std::uint64_t& data,std::int32_t& count)const {
        const auto capacity=value<std::int32_t>(header+12);
        return get(header,data)&&get(header+8,count)&&count>=0&&count<=8192&&capacity>=count&&capacity<=65536&&
               (!count||(data>=0x10000&&!(data&7)));
    }
    std::string raw(std::uint64_t address,unsigned size)const {
        std::vector<std::uint8_t> bytes(size);if(!memory_.copy(address,bytes.data(),bytes.size()))return "null";
        static const char digits[]="0123456789abcdef";std::string text;text.reserve(size*2);
        for(auto b:bytes){text+=digits[b>>4];text+=digits[b&15];}return quote(text);
    }
public:
    NetworkRuntimeDecoder(const MemoryReader& memory,const std::map<std::uint64_t,NetworkRuntimeClass>& classes,
                          const std::map<std::uint64_t,NetworkRuntimeField>& fields,const std::function<void()>& check)
        :memory_(memory),classes_(classes),fields_(fields),check_(check){for(const auto& p:classes)classIndices_[p.second.index]=&p.second;}
    std::string decodeCache(std::uint64_t address,NetworkRuntimeCacheRecord* decoded=nullptr)const {
        // FClassNetCache: FieldsBase, Super, weak Class, checksum, Fields.
        std::array<std::uint8_t,48> readableHeader{};
        if(!memory_.copy(address,readableHeader.data(),readableHeader.size()))return {};
        const auto index=value<std::int32_t>(address+16),serial=value<std::int32_t>(address+20);
        const auto found=classIndices_.find(index);
        if(found==classIndices_.end()||serial<1||serial>0x3fffffff)return {};
        const auto& cls=*found->second;
        const auto base=value<std::int32_t>(address);
        const auto super=value<std::uint64_t>(address+8);
        std::uint64_t data=0;std::int32_t count=0;
        if(base<0||base>1000000||!cacheArray(address+32,data,count)||base+count>1000000)return {};
        const NetworkRuntimeClass* superClass=nullptr;
        if(super){
            const auto parentIndex=value<std::int32_t>(super+16);
            const auto parent=classIndices_.find(parentIndex);
            if(parent==classIndices_.end()||parent->second->path!=cls.super)return {};
            // Parent caches may have no local fields.
            const auto parentBase=value<std::int32_t>(super),n=value<std::int32_t>(super+40);
            const auto parentSerial=value<std::int32_t>(super+20);
            if(parentSerial<=0||parentSerial>0x3fffffff||parentBase<0||n<0||n>8192||parentBase+n!=base)return {};
            superClass=parent->second;
        }else if(base!=0||(!count&&!cls.super.empty()))return {};
        for(const unsigned stride:{24u,16u}){
            bool valid=true;std::string rows;std::vector<NetworkRuntimeCacheEntry> entries;
            for(int i=0;i<count;++i){
                if((i&127)==0)check_();
                const auto at=data+(std::uint64_t)i*stride;
                const auto* f=field(value<std::uint64_t>(at));
                const auto netIndex=value<std::int32_t>(at+8);const auto checksum=value<std::uint32_t>(at+12);
                const auto incompatible=stride==24?value<std::uint8_t>(at+16):0;
                if(!f||f->owner!=cls.path||netIndex!=base+i||incompatible>1){valid=false;break;}
                if(i)rows+=',';
                rows+="{\"path\":"+quote(f->path)+",\"field_net_index\":"+std::to_string(netIndex)+",\"checksum\":"+std::to_string(checksum)+",\"incompatible\":"+(incompatible?"true":"false")+",\"record_address\":"+quote(hex(at))+",\"record_bytes\":"+raw(at,stride)+"}";
                entries.push_back({f->path,netIndex,checksum,incompatible!=0});
            }
            if(valid){
                const auto fieldStride=count?stride:24u;
                const auto classChecksum=value<std::uint32_t>(address+24);
                const auto superPath=superClass?quote(superClass->path):"null";
                auto json="{\"kind\":\"class_net_cache\",\"class\":"+quote(cls.path)+",\"address\":"+quote(hex(address))+",\"header_bytes\":"+raw(address,48)+",\"weak_class_index\":"+std::to_string(index)+",\"weak_class_serial\":"+std::to_string(serial)+",\"super_address\":"+quote(hex(super))+",\"super_class\":"+superPath+",\"fields_base\":"+std::to_string(base)+",\"field_count\":"+std::to_string(count)+",\"field_index_start\":"+std::to_string(base)+",\"field_index_end_exclusive\":"+std::to_string(base+count)+",\"field_index_end_inclusive\":"+(count?std::to_string(base+count-1):"null")+",\"field_stride\":"+std::to_string(fieldStride)+",\"class_checksum\":"+std::to_string(classChecksum)+",\"entries\":["+rows+"],\"validation\":\"runtime_cross_checked\"}";
                if(decoded){decoded->classPath=cls.path;decoded->superClassPath=superClass?superClass->path:"";decoded->address=address;decoded->superAddress=super;decoded->fieldsBase=base;decoded->fieldCount=count;decoded->fieldIndexEndExclusive=base+count;decoded->fieldStride=fieldStride;decoded->classChecksum=classChecksum;decoded->entries=std::move(entries);decoded->json=json;}
                return json;
            }
        }
        return {};
    }
    std::string decodeLayout(std::uint64_t address,NetworkLayoutStorage* storage=nullptr)const {
        std::array<std::uint8_t,512> header{};if(!memory_.copy(address,header.data(),header.size()))return {};
        // The owner position can vary with engine options; require the same
        // class to own every parent property and all parent/command ranges.
        std::set<std::uint64_t> possibleClasses;
        for(unsigned off=0;off+8<=header.size();off+=8){std::uint64_t p=0;std::memcpy(&p,header.data()+off,8);if(classes_.count(p))possibleClasses.insert(p);}
        if(possibleClasses.empty())return {};
        for(unsigned offset=16;offset<=256;offset+=8){
            std::uint64_t parents=0,commands=0;std::int32_t parentCount=0,commandCount=0;
            if(!array(address+offset,parents,parentCount)||!array(address+offset+16,commands,commandCount)||parentCount>2048||commandCount>8192)continue;
            const auto* first=field(value<std::uint64_t>(parents));if(!first||first->rpc)continue;
            for(const auto ownerAddress:possibleClasses){
                const auto& cls=classes_.at(ownerAddress);if(!belongs(first->owner,cls))continue;
                for(unsigned parentStride:{48u,56u,64u,72u})for(unsigned cmdStride:{32u,40u})for(unsigned rangesOffset:{28u,24u}){
                    bool valid=true;std::vector<const NetworkRuntimeField*> top;
                    std::vector<std::pair<std::uint16_t,std::uint16_t>> commandRanges;
                    std::string parentRows;
                    for(int p=0;p<parentCount;++p){
                        if((p&63)==0)check_();const auto at=parents+(std::uint64_t)p*parentStride;
                        const auto* f=field(value<std::uint64_t>(at));
                        const auto start=value<std::uint16_t>(at+rangesOffset),end=value<std::uint16_t>(at+rangesOffset+2);
                        const auto arrayIndex=value<std::int32_t>(at+16);
                        const auto condition=value<std::uint8_t>(at+32);
                        if(!f||f->rpc||!belongs(f->owner,cls)||arrayIndex<0||arrayIndex>=f->arrayDim||start>end||end>commandCount||condition>32){valid=false;break;}
                        top.push_back(f);commandRanges.push_back({start,end});if(p)parentRows+=',';
                        parentRows+="{\"path\":"+quote(f->path)+",\"parent_index\":"+std::to_string(p)+",\"array_index\":"+std::to_string(arrayIndex)+",\"cmd_start\":"+std::to_string(start)+",\"cmd_end\":"+std::to_string(end)+",\"lifetime_condition\":"+std::to_string(condition)+",\"record_bytes\":"+raw(at,parentStride)+"}";
                    }
                    if(!valid)continue;
                    std::string commandRows;unsigned handles=0;
                    for(int c=0;c<commandCount;++c){
                        if((c&127)==0)check_();const auto at=commands+(std::uint64_t)c*cmdStride;
                        const auto type=value<std::uint8_t>(at+28);
                        const auto* f=field(value<std::uint64_t>(at));
                        const auto parent=value<std::uint16_t>(at+22),handle=value<std::uint16_t>(at+20);
                        const auto memoryOffset=value<std::int32_t>(at+12);
                        const auto end=value<std::uint16_t>(at+8);
                        if(type==1){if(c)commandRows+=',';commandRows+="{\"cmd_index\":"+std::to_string(c)+",\"type\":1,\"return\":true,\"record_bytes\":"+raw(at,cmdStride)+"}";continue;}
                        if(!f||f->rpc||parent>=top.size()||handle<1||type>48||memoryOffset<0||memoryOffset>268435456||(type==0&&(end<=c||end>commandCount))){valid=false;break;}
                        if(c<commandRanges[parent].first||c>=commandRanges[parent].second){valid=false;break;}
                        if(top[parent]->address==f->address&&memoryOffset!=f->offset+value<std::int32_t>(parents+(std::uint64_t)parent*parentStride+16)*f->elementSize){valid=false;break;}
                        if(c)commandRows+=',';++handles;
                        commandRows+="{\"path\":"+quote(f->path)+",\"parent_path\":"+quote(top[parent]->path)+",\"cmd_index\":"+std::to_string(c)+",\"relative_handle\":"+std::to_string(handle)+",\"parent_index\":"+std::to_string(parent)+",\"offset\":"+std::to_string(memoryOffset)+",\"type\":"+std::to_string(type)+",\"end_cmd\":"+std::to_string(end)+",\"checksum\":"+std::to_string(value<std::uint32_t>(at+24))+",\"record_address\":"+quote(hex(at))+",\"record_bytes\":"+raw(at,cmdStride)+"}";
                    }
                    if(valid&&handles){
                        unsigned ownerOffset=0;
                        for(;ownerOffset+8<=header.size();ownerOffset+=8){std::uint64_t p=0;std::memcpy(&p,header.data()+ownerOffset,8);if(p==ownerAddress)break;}
                        if(storage)*storage={parents,commands,(unsigned)parentCount,(unsigned)commandCount,parentStride,cmdStride};
                        return "{\"kind\":\"rep_layout\",\"class\":"+quote(cls.path)+",\"address\":"+quote(hex(address))+",\"owner_pointer_address\":"+quote(hex(address+ownerOffset))+",\"header_bytes\":"+raw(address,512)+",\"parents_offset\":"+std::to_string(offset)+",\"parent_stride\":"+std::to_string(parentStride)+",\"parent_ranges_offset\":"+std::to_string(rangesOffset)+",\"command_stride\":"+std::to_string(cmdStride)+",\"parents\":["+parentRows+"],\"commands\":["+commandRows+"],\"validation\":\"runtime_cross_checked\"}";
                    }
                }
            }
        }
        return {};
    }
    NetworkRuntimeResult scan(const std::vector<std::uint64_t>& roots,const Module& module,unsigned budgetMs=15000,
                              const std::set<std::uint64_t>& objects={},const std::map<std::uint64_t,unsigned>& rootSizes={})const {
        constexpr std::size_t maxNodes=32768,maxQueuedNodes=65536;
        constexpr std::size_t maxBytes=16*1024*1024;
        NetworkRuntimeResult result;std::set<std::uint64_t> cacheSeen,layoutSeen;
        std::map<std::uint64_t,unsigned> queued,visited;
        // Store each address range once. Overlapping graph reads previously
        // consumed nearly half the evidence budget without adding bytes.
        std::map<std::uint64_t,std::uint64_t> covered;
        const auto exhausted=[&](const char* reason){result.budgetExhausted=true;result.budgetReasons.insert(reason);};
        const auto saveBytes=[&](std::uint64_t address,const std::vector<std::uint8_t>& bytes){
            const auto end=address+bytes.size();auto at=address;
            std::vector<std::pair<std::uint64_t,std::uint64_t>> missing;
            auto it=covered.upper_bound(at);if(it!=covered.begin())--it;
            for(;it!=covered.end()&&it->first<end;++it){
                if(it->second<=at)continue;
                if(it->first>at)missing.push_back({at,std::min(end,it->first)});
                at=std::max(at,it->second);if(at>=end)break;
            }
            if(at<end)missing.push_back({at,end});
            std::size_t extra=0;for(const auto& span:missing)extra+=(std::size_t)(span.second-span.first);
            if(extra>maxBytes-result.bytes){exhausted("bytes");return false;}
            for(const auto& span:missing){
                const auto begin=bytes.begin()+(std::size_t)(span.first-address);
                result.blocks.push_back({span.first,std::vector<std::uint8_t>(begin,begin+(std::size_t)(span.second-span.first))});
            }
            result.bytes+=extra;
            auto first=address,last=end;it=covered.lower_bound(first);
            if(it!=covered.begin()){auto previous=std::prev(it);if(previous->second>=first)it=previous;}
            while(it!=covered.end()&&it->first<=last){first=std::min(first,it->first);last=std::max(last,it->second);it=covered.erase(it);}
            covered[first]=last;return true;
        };
        struct Node{std::uint64_t address;unsigned depth,size;};std::vector<Node> queue;
        for(auto root:roots){
            const auto found=rootSizes.find(root);
            const auto size=found==rootSizes.end()?4096u:std::max(1u,std::min(found->second,65536u));
            if(queued[root]>=size)continue;queued[root]=size;queue.push_back({root,0,size});
        }
        result.roots=queue.size();
        const auto deadline=GetTickCount64()+budgetMs;
        const auto capture=[&](std::uint64_t address,unsigned size){
            if(!size||size>1024*1024)return false;
            std::vector<std::uint8_t> bytes(size);
            if(!memory_.copy(address,bytes.data(),size)){++result.readFailures;return false;}
            return saveBytes(address,bytes);
        };
        for(std::size_t n=0;n<queue.size()&&result.nodes<maxNodes;++n){
            check_();if(GetTickCount64()>=deadline){exhausted("time");break;}
            const auto node=queue[n];if(visited[node.address]>=node.size)continue;
            visited[node.address]=node.size;
            std::vector<std::uint8_t> bytes(node.size);
            if(!memory_.copy(node.address,bytes.data(),bytes.size())){++result.readFailures;continue;}
            ++result.nodes;if(!saveBytes(node.address,bytes))break;
            // Pointer traversal includes the tail of small allocations; cache
            // header probes separately require their full 48 bytes.
            for(unsigned off=0;off+8<=bytes.size();off+=8){
                if((off&1023)==0){check_();if(GetTickCount64()>=deadline){exhausted("time");return result;}}
                std::int32_t weakIndex=0,serial=0;
                if(off+48<=bytes.size()){std::memcpy(&weakIndex,bytes.data()+off+16,4);std::memcpy(&serial,bytes.data()+off+20,4);}
                // An empty standalone header has too little evidence to be a
                // cache. Empty ancestors are accepted only through a validated
                // nonempty child's Super link below.
                if(serial>0&&classIndices_.count(weakIndex)&&value<std::int32_t>(node.address+off+40)>0){
                    auto address=node.address+off;
                    std::set<std::uint64_t> chain;
                    for(unsigned depth=0;address&&depth<64&&chain.insert(address).second;++depth){
                        if(cacheSeen.count(address))break;
                        NetworkRuntimeCacheRecord record;
                        if(decodeCache(address,&record).empty())break;
                        const auto data=value<std::uint64_t>(address+32);
                        if(!capture(address,48)||(record.fieldCount&&!capture(data,(unsigned)record.fieldCount*record.fieldStride))){
                            ++result.incompleteRecords;break;
                        }
                        cacheSeen.insert(address);
                        result.caches.push_back(record.json);
                        address=record.superAddress;
                        result.cacheRecords.push_back(std::move(record));
                    }
                }
                std::uint64_t pointer=0;std::memcpy(&pointer,bytes.data()+off,8);
                if(classes_.count(pointer)&&node.depth>0){
                    // Owner need not be first: inspect nearby object starts.
                    for(unsigned back=0;back<=256&&back<=off;back+=8){
                        const auto address=node.address+off-back;
                        if(layoutSeen.count(address))continue;
                        NetworkLayoutStorage storage;auto layout=decodeLayout(address,&storage);
                        if(!layout.empty()){
                            layoutSeen.insert(address);
                            // Only publish a record after its exact header,
                            // Parents and Cmds can all be replayed offline.
                            if(capture(address,512)&&capture(storage.parents,storage.parentCount*storage.parentStride)&&
                               capture(storage.commands,storage.commandCount*storage.commandStride))result.layouts.push_back(layout);
                            else ++result.incompleteRecords;
                            break;
                        }
                    }
                }
                if(node.depth>=4||queue.size()>=maxQueuedNodes||pointer<0x10000||pointer>=0x0000800000000000ull||(pointer&7)||
                   (pointer>=module.base&&pointer<module.base+module.size)||classes_.count(pointer)||objects.count(pointer)||field(pointer))continue;
                unsigned size=512;
                std::int32_t count=0,capacity=0;
                if(off+16<=bytes.size()){std::memcpy(&count,bytes.data()+off+8,4);std::memcpy(&capacity,bytes.data()+off+12,4);}
                if(count>0&&count<=2048&&capacity>=count&&capacity<=4096)size=(unsigned)std::min<std::uint64_t>((std::uint64_t)capacity*72,65536);
                if(queued[pointer]>=size)continue;queued[pointer]=size;
                queue.push_back({pointer,node.depth+1,size});
            }
        }
        if(result.nodes>=maxNodes)exhausted("nodes");
        if(queue.size()>=maxQueuedNodes)exhausted("queue");
        return result;
    }
};
} // namespace ebon
