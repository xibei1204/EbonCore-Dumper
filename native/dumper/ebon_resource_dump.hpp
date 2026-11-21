#pragma once
#include "../sdk/ebon_reflection.hpp"
#include "ebon_resource_categories.hpp"
#include <cmath>
#include <limits>
#include <type_traits>

namespace ebon::resources {

// No ProcessEvent, asset loading, game-thread calls or writes to target memory.
// Native containers are accepted only after their allocation bitmap and every
// live entry validate. A failed read remains an explicit unresolved value.
struct ArrayHeader {
    std::uint64_t data=0;
    std::int32_t num=0,capacity=0;
    bool operator==(const ArrayHeader& b)const{return data==b.data&&num==b.num&&capacity==b.capacity;}
};
static_assert(sizeof(ArrayHeader)==16);
struct SparseView {
    ArrayHeader header;
    std::vector<std::int32_t> live;
};
struct Descriptor {
    std::uint64_t field=0,inner=0,key=0,value=0;
    std::string type;
    std::int32_t size=0;
    const Type* target=nullptr;
    unsigned alignment=0,mask=0,byteOffset=0;
};

class Dumper {
    const MemoryReader& memory_;
    Reflection& reflection_;
    const Config& config_;
    std::filesystem::path output_;
    std::map<std::uint64_t,const Type*> types_;
    std::map<std::string,const Type*> paths_;
    std::map<std::uint64_t,const Object*> objects_;
    std::map<std::uint64_t,Descriptor> descriptors_;
    std::map<std::string,std::ofstream> indexes_;
    std::ofstream manifest_,issues_,textValues_,references_;
    std::string location_;
    std::uint64_t values_=0,bytes_=0,maxValues_=0,maxBytes_=0;
    ULONGLONG deadline_=0;
    std::int32_t maxEntries_=0,maxString_=0;
    unsigned maxDepth_=0;
    std::size_t unresolved_=0,resources_=0,rows_=0,texts_=0;

    std::map<std::string,std::size_t> counts_;
    struct CapturedBody {std::string file,kind;std::size_t rows=0,issues=0;};
    struct AssetReference {std::string root,source,target,kind;};
    std::map<std::string,CapturedBody> capturedBodies_;
    std::vector<AssetReference> assetReferences_;
    std::set<std::string> battleObjects_;
    std::string captureRoot_;
    bool captureReferences_=false;

    std::int64_t option(const char* key,std::int64_t fallback,std::int64_t low,std::int64_t high)const{
        const auto n=config_.number(key,fallback);
        if(n<low||n>high)throw std::runtime_error(std::string(key)+" outside supported bounds");
        return n;
    }
    void checkpoint()const{
        if(GetTickCount64()>=deadline_)throw std::runtime_error("resource capture deadline exceeded");
        if(values_>=maxValues_)throw std::runtime_error("resource value budget exceeded");
    }
    void bufferBudget(const std::string& value)const{
        if(value.size()+1>maxBytes_-bytes_)throw std::runtime_error("resource output byte budget exceeded");
    }
    void line(std::ofstream& out,const std::string& value){
        if(value.size()+1>maxBytes_-bytes_)throw std::runtime_error("resource output byte budget exceeded");
        out<<value<<'\n';
        if(!out)throw std::runtime_error("resource output write failed");
        bytes_+=value.size()+1;
    }
    std::string raw(std::uint64_t address,std::size_t size)const{
        size=std::min<std::size_t>(size,64);
        std::vector<unsigned char> buffer(size);
        if(size&&!memory_.copy(address,buffer.data(),size))return "null";
        static const char digits[]="0123456789abcdef";
        std::string result;for(auto b:buffer){result+=digits[b>>4];result+=digits[b&15];}
        return quote(result);
    }
    std::string unavailable(const std::string& reason,std::uint64_t address=0,std::size_t size=0){
        ++unresolved_;
        if(issues_.is_open()&&unresolved_<=1000)
            line(issues_,"{\"reason\":"+quote(reason)+",\"address\":"+quote(hex(address))+"}");
        return "{\"_unresolved\":"+quote(reason)+",\"address\":"+quote(hex(address))+",\"raw_hex\":"+raw(address,size)+"}";
    }
    bool header(std::uint64_t address,ArrayHeader& h)const{
        return memory_.get(address,h)&&h.num>=0&&h.capacity>=h.num&&h.capacity<=maxEntries_*16&&
            h.num<=maxEntries_&&(!h.num||(h.data>=0x10000&&!(h.data&3)));
    }
    bool string(std::uint64_t address,std::string& result)const{
        ArrayHeader h;
        if(!memory_.get(address,h)||h.num<0||h.num>maxString_||h.capacity<h.num||h.capacity>maxString_*16)return false;
        if(!h.num){result.clear();return true;}
        if(h.data<0x10000||(h.data&1))return false;
        std::wstring text(h.num,L'\0');
        if(!memory_.copy(h.data,text.data(),text.size()*2)||text.back()!=0)return false;
        text.pop_back();
        if(text.find(L'\0')!=text.npos)return false;
        try{result=utf8(text);}catch(...){return false;}
        ArrayHeader after;return memory_.get(address,after)&&h==after;
    }
    bool isA(const Type* type,const std::string& base)const{
        std::set<std::string> seen;
        for(;type&&seen.insert(type->path).second;){
            if(type->path==base)return true;
            auto i=paths_.find(type->super);type=i==paths_.end()?nullptr:i->second;
        }
        return false;
    }
    bool hasFields(const Type* type)const{
        std::set<std::string> seen;
        for(;type&&seen.insert(type->path).second;){
            if(!type->fields.empty())return true;
            auto i=paths_.find(type->super);type=i==paths_.end()?nullptr:i->second;
        }
        return false;
    }
    bool current(const Object& o)const{
        std::int32_t index=-1;std::uint64_t cls=0;
        return reflection_.read(o.address,reflection_.layout().objIndex,index)&&index==o.index&&
            reflection_.read(o.address,reflection_.layout().objClass,cls)&&cls==o.klass&&
            reflection_.name(o.address,reflection_.layout().objName)==o.name;
    }
    unsigned alignment(const Descriptor& d)const{
        if(d.alignment)return d.alignment;
        if(d.target&&d.type=="StructProperty"&&d.target->alignment)return *d.target->alignment;
        if(d.type=="NameProperty")return 4;
        if(d.type=="StructProperty")return 0; // Never guess a struct's native map alignment.
        if(d.type=="BoolProperty"||d.type=="ByteProperty"||d.type=="Int8Property")return 1;
        if(d.type=="Int16Property"||d.type=="UInt16Property")return 2;
        if(d.type=="EnumProperty"&&(d.size==1||d.size==2||d.size==4||d.size==8))return (unsigned)d.size;
        if(d.type=="IntProperty"||d.type=="UInt32Property"||d.type=="FloatProperty")return 4;
        return 8;
    }
    static std::uint64_t align(std::uint64_t n,unsigned a){return (n+a-1)/a*a;}
    std::filesystem::path bodyPath(const std::string& identity)const{
        if(identity.empty()||identity.front()!='/'||identity.find('\\')!=std::string::npos)
            throw std::runtime_error("invalid resource object path: "+identity);
        const auto firstDot=identity.find('.');
        const auto package=identity.substr(1,firstDot==std::string::npos?std::string::npos:firstDot-1);
        if(package.empty()||package.front()=='/'||package.back()=='/'||package.find('/')==std::string::npos)
            throw std::runtime_error("invalid resource package path: "+identity);
        std::filesystem::path result="data";
        const auto lastSlash=package.rfind('/');
        const auto directory=package.substr(0,lastSlash);
        const auto firstSlash=package.find('/');
        std::size_t start=firstSlash!=std::string::npos&&package.substr(0,firstSlash)=="Game"?firstSlash+1:0;
        const auto validateComponent=[&](const std::string& part){
            if(part.empty()||part=="."||part==".."||part.back()=='.'||part.back()==' '||
               part.find_first_of("\\/:*?\"<>|")!=std::string::npos)
                throw std::runtime_error("unsafe resource path component: "+identity);
            std::string stem=part.substr(0,part.find('.'));
            for(auto& c:stem)if(c>='a'&&c<='z')c=static_cast<char>(c-'a'+'A');
            bool reserved=stem=="CON"||stem=="PRN"||stem=="AUX"||stem=="NUL";
            if(stem.size()==4&&(stem.rfind("COM",0)==0||stem.rfind("LPT",0)==0)&&stem[3]>='1'&&stem[3]<='9')
                reserved=true;
            if(reserved)throw std::runtime_error("reserved device name in resource path: "+identity);
        };
        while(start<directory.size()){
            const auto slash=directory.find('/',start);
            const auto part=directory.substr(start,slash==std::string::npos?std::string::npos:slash-start);
            validateComponent(part);
            result/=part;
            if(slash==std::string::npos)break;
            start=slash+1;
        }
        auto objectSuffix=firstDot==std::string::npos?std::string():identity.substr(firstDot+1);
        const auto packageName=package.substr(package.rfind('/')+1);
        if(objectSuffix==packageName)objectSuffix.clear();
        else if(objectSuffix.rfind(packageName+".",0)==0)objectSuffix.erase(0,packageName.size()+1);
        if(!objectSuffix.empty()){
            std::size_t objectStart=0;
            while(objectStart<objectSuffix.size()){
                const auto dot=objectSuffix.find('.',objectStart);
                auto part=objectSuffix.substr(objectStart,dot==std::string::npos?std::string::npos:dot-objectStart);
                for(std::size_t colon=part.find(':');colon!=std::string::npos;colon=part.find(':',colon+3))
                    part.replace(colon,1,"%3A");
                validateComponent(part);
                objectSuffix.replace(objectStart, dot==std::string::npos?objectSuffix.size()-objectStart:dot-objectStart,part);
                if(dot==std::string::npos)break;
                objectStart+=part.size()+1;
            }
        }
        result/=packageName+(objectSuffix.empty()?std::string():"."+objectSuffix)+".jsonl";
        // A widget's Outer chain can exceed the filesystem's per-component
        // limit even when the capture directory is short. Keep its identity
        // in the JSON/manifest and use a stable short filename in that case.
        if(result.native().size()>180){
            std::uint64_t hash=14695981039346656037ull;
            for(const unsigned char byte:identity){hash^=byte;hash*=1099511628211ull;}
            std::ostringstream name;name<<"object_"<<std::hex<<std::setfill('0')<<std::setw(16)<<hash<<".jsonl";
            return std::filesystem::path("data")/"_long"/name.str();
        }
        return result;
    }
    std::string prepareBodyPath(const std::string& identity)const{
        const auto relative=bodyPath(identity);
        const auto destination=output_/relative;
        std::filesystem::create_directories(destination.parent_path());
        if(std::filesystem::exists(destination))
            throw std::runtime_error("resource output path collision: "+relative.generic_string());
        return relative.generic_string();
    }
    Descriptor describe(std::uint64_t field){
        auto cached=descriptors_.find(field);if(cached!=descriptors_.end())return cached->second;
        Descriptor d;d.field=field;
        const auto& l=reflection_.layout();std::uint64_t cls=0;
        if(!reflection_.read(field,l.fieldClass,cls))return d;
        d.type=reflection_.name(cls,l.fieldClassName);
        if(!reflection_.read(field,l.propElement,d.size)||d.size<1||d.size>l.maxTypeSize){d.type.clear();return d;}
        const auto extra=l.propOffset+0x2c;
        if(d.type=="StructProperty"||d.type=="EnumProperty"||d.type=="ByteProperty"){
            std::set<const Type*> candidates;
            for(auto off=extra;off<=extra+32;off+=8){
                std::uint64_t p=0;if(!reflection_.read(field,off,p))continue;
                auto i=types_.find(p);if(i==types_.end())continue;
                if((d.type=="StructProperty"&&i->second->kind=="struct"&&i->second->size==d.size)||
                   (d.type!="StructProperty"&&i->second->kind=="enum"))candidates.insert(i->second);
            }
            if(candidates.size()==1)d.target=*candidates.begin();
        }
        if(d.type=="BoolProperty"){
            unsigned char b[4]{};
            if(memory_.copy(field+extra,b,4)&&b[0]>=1&&b[0]<=8&&b[1]<b[0]&&b[2]&&!(b[2]&(b[2]-1))&&
               b[3]&&(b[3]==255||b[3]==b[2])){d.mask=b[3];d.byteOffset=b[1];}
        }
        if(d.type=="ArrayProperty"){
            for(auto off=extra;off<=extra+32;off+=8){
                std::uint64_t p=0,owner=0,childClass=0;
                if(!reflection_.read(field,off,p)||!reflection_.read(p,16,owner)||(owner&~std::uint64_t(1))!=field||
                   !reflection_.read(p,l.fieldClass,childClass))continue;
                const auto type=reflection_.name(childClass,l.fieldClassName);
                if(type.size()>8&&type.substr(type.size()-8)=="Property"){d.inner=p;break;}
            }
        }
        if(d.type=="MapProperty"){reflection_.read(field,extra,d.key);reflection_.read(field,extra+8,d.value);}
        if(d.type=="SetProperty"||d.type=="EnumProperty")reflection_.read(field,extra,d.inner);
        descriptors_[field]=d;return d;
    }
    template<class T> std::string number(std::uint64_t address){
        T v{};if(!memory_.get(address,v))return unavailable("unreadable numeric value",address,sizeof(T));
        if constexpr(std::is_floating_point_v<T>){if(!std::isfinite(v))return unavailable("non-finite numeric value",address,sizeof(T));}
        std::ostringstream s;s<<std::setprecision(std::numeric_limits<T>::max_digits10?std::numeric_limits<T>::max_digits10:20);
        if constexpr(std::is_same_v<T,std::int8_t>||std::is_same_v<T,std::uint8_t>)s<<(int)v;else s<<v;
        return s.str();
    }
    std::string structure(std::uint64_t address,const Type& type,unsigned depth,std::set<std::string> lineage={}){
        if(depth>maxDepth_)return unavailable("struct depth limit",address,type.size.value_or(0));
        if(!lineage.insert(type.path).second)return unavailable("struct inheritance cycle",address);
        // FInstancedStruct stores the dynamic UScriptStruct and owned memory.
        if(type.path=="/Script/CoreUObject.InstancedStruct"&&type.size==16){
            std::uint64_t schema=0,data=0;
            if(!memory_.get(address,schema)||!memory_.get(address+8,data))return unavailable("unreadable instanced struct",address,16);
            if(!schema&&!data)return "null";
            auto i=types_.find(schema);
            if(!data||i==types_.end()||i->second->kind!="struct")return unavailable("unknown instanced struct schema",address,16);
            return "{\"_type\":"+quote(i->second->path)+",\"value\":"+structure(data,*i->second,depth+1)+"}";
        }
        std::string result="{";bool comma=false;
        auto parent=paths_.find(type.super);
        if(type.kind=="struct"&&type.size&&*type.size>0&&type.fields.empty()&&
           (parent==paths_.end()||!hasFields(parent->second)))
            return unavailable("native struct has no reflected values: "+type.path,address,*type.size);
        if(parent!=paths_.end()&&hasFields(parent->second)){
            result+="\"_super\":"+structure(address,*parent->second,depth+1,lineage);comma=true;
        }
        for(const auto& p:type.fields){
            checkpoint();
            const auto parentLocation=location_;location_+='.'+p.name;
            if(comma)result+=',';comma=true;result+=quote(p.name)+':';
            if(p.offset<0||p.element<1||p.array<1||!type.size||
               std::int64_t(p.offset)+std::int64_t(p.element)*p.array>*type.size){result+=unavailable("property outside struct",address);location_=parentLocation;continue;}
            auto d=describe(p.address);
            if(p.array>maxEntries_){result+=unavailable("fixed array entry budget exceeded",address+p.offset);location_=parentLocation;continue;}
            if(p.array>1)result+='[';
            for(int n=0;n<p.array;++n){
                if(n)result+=',';result+=value(address+p.offset+std::uint64_t(n)*p.element,d,depth+1);bufferBudget(result);
            }
            if(p.array>1)result+=']';
            bufferBudget(result);
            location_=parentLocation;
        }
        if(!type.complete){if(comma)result+=',';result+="\"_schema\":"+unavailable("incomplete reflected schema",type.address);}
        return result+"}";
    }
    std::string text(std::uint64_t address,const Descriptor& d){
        // FText is 16 bytes in this build. No virtual calls from this worker.
        std::string result=unavailable("FText native layout unverified; candidates are not confirmed translations",address,d.size);
        result.pop_back();result+=",\"candidates\":[";
        std::uint64_t data=0;memory_.get(address,data);
        bool comma=false;std::set<std::string> seen;
        if(data>=0x10000&&!(data&7))for(unsigned off=8;off<128;off+=8){
            std::string candidate;
            if(string(data+off,candidate)&&!candidate.empty()&&seen.insert(candidate).second){
                if(comma)result+=',';comma=true;
                result+="{\"offset\":"+std::to_string(off)+",\"text\":"+quote(candidate)+"}";
                bufferBudget(result);
            }
            // Some text data uses a shared FString rather than an inline one.
            // Preserve its route as a candidate instead of guessing semantics.
            std::uint64_t shared=0;
            if(off<=48&&memory_.get(data+off,shared)&&shared>=0x10000&&!(shared&7)&&string(shared,candidate)&&
               !candidate.empty()&&seen.insert(candidate).second){
                if(comma)result+=',';comma=true;
                result+="{\"pointer_offset\":"+std::to_string(off)+",\"text\":"+quote(candidate)+"}";
                bufferBudget(result);
            }
        }
        result+="]}";
        if(textValues_.is_open()){line(textValues_,"{\"source\":"+quote(location_)+",\"kind\":\"FText\",\"value\":"+result+"}");++texts_;}
        return result;
    }
    std::string container(std::uint64_t address,const Descriptor& d,unsigned depth){
        if(d.type=="ArrayProperty"){
            const auto inner=describe(d.inner);ArrayHeader h;
            if(inner.type.empty()||!header(address,h))return unavailable("invalid array metadata/header",address,d.size);
            std::string result="[";
            const auto parentLocation=location_;
            for(int n=0;n<h.num;++n){
                location_=parentLocation+"["+std::to_string(n)+"]";
                if(n)result+=',';result+=value(h.data+std::uint64_t(n)*inner.size,inner,depth+1);bufferBudget(result);
            }
            location_=parentLocation;
            ArrayHeader after;
            if(!memory_.get(address,after)||!(h==after))return unavailable("array changed during capture",address,d.size);
            return result+"]";
        }
        const auto key=describe(d.type=="SetProperty"?d.inner:d.key);
        const auto val=d.type=="MapProperty"?describe(d.value):Descriptor{};
        const auto ka=alignment(key),va=d.type=="MapProperty"?alignment(val):1;
        if(key.type.empty()||!ka||!va||(d.type=="MapProperty"&&val.type.empty()))
            return unavailable("unknown set/map property layout or alignment",address,d.size);
        const auto pairAlignment=std::max(ka,va);
        const auto valueOffset=align(key.size,va);
        const auto pairSize=align(d.type=="MapProperty"?valueOffset+val.size:key.size,pairAlignment);
        const auto stride=align(pairSize+8,std::max(4u,pairAlignment));
        SparseView view;
        if(!sparse(address,view))return unavailable("invalid sparse set/map allocation bitmap",address,d.size);
        std::string result="[";bool comma=false;
        const auto parentLocation=location_;
        for(auto n:view.live){
            if(comma)result+=',';comma=true;const auto at=view.header.data+std::uint64_t(n)*stride;
            location_=parentLocation+"["+std::to_string(n)+"]";
            if(d.type=="MapProperty"){
                location_+=".key";const auto keyValue=value(at,key,depth+1);
                location_=parentLocation+"["+std::to_string(n)+"].value";
                result+="{\"key\":"+keyValue+",\"value\":"+value(at+valueOffset,val,depth+1)+"}";
            }else result+=value(at,key,depth+1);
            bufferBudget(result);
        }
        location_=parentLocation;
        SparseView after;
        if(!sparse(address,after)||!(view.header==after.header)||view.live!=after.live)
            return unavailable("set/map changed during capture",address,d.size);
        return result+"]";
    }
    std::string value(std::uint64_t address,const Descriptor& d,unsigned depth){
        ++values_;checkpoint();
        if(depth>maxDepth_)return unavailable("value depth limit",address,d.size);
        if(d.type=="IntProperty")return number<std::int32_t>(address);
        if(d.type=="Int64Property")return number<std::int64_t>(address);
        if(d.type=="UInt64Property")return number<std::uint64_t>(address);
        if(d.type=="UInt32Property")return number<std::uint32_t>(address);
        if(d.type=="UInt16Property")return number<std::uint16_t>(address);
        if(d.type=="Int16Property")return number<std::int16_t>(address);
        if(d.type=="Int8Property")return number<std::int8_t>(address);
        if(d.type=="FloatProperty")return number<float>(address);
        if(d.type=="DoubleProperty")return number<double>(address);
        if(d.type=="ByteProperty"&&!d.target)return number<std::uint8_t>(address);
        if(d.type=="EnumProperty"||(d.type=="ByteProperty"&&d.target)){
            std::int64_t v=0;
            if(d.size>8||!memory_.copy(address,&v,d.size))return unavailable("invalid enum value",address,d.size);
            if(d.type=="EnumProperty"&&d.size<8){const unsigned bits=d.size*8;v=(v^std::int64_t(1ull<<(bits-1)))-std::int64_t(1ull<<(bits-1));}
            std::string name;
            if(d.target)for(const auto& e:d.target->enumValues)if(e.second==v){name=e.first;break;}
            return "{\"value\":"+std::to_string(v)+",\"name\":"+(name.empty()?"null":quote(name))+"}";
        }
        if(d.type=="BoolProperty"){
            std::uint8_t v=0;
            if(!d.mask||d.byteOffset>=unsigned(d.size)||!memory_.get(address+d.byteOffset,v))return unavailable("unverified bool mask",address,d.size);
            return (v&d.mask)?"true":"false";
        }
        if(d.type=="NameProperty"){
            const auto n=reflection_.name(address,0);return n.empty()?unavailable("invalid FName",address,d.size):quote(n);
        }
        if(d.type=="StrProperty"){
            std::string s;
            if(!string(address,s))return unavailable("invalid FString",address,d.size);
            if(!s.empty()&&textValues_.is_open()){
                line(textValues_,"{\"source\":"+quote(location_)+",\"kind\":\"FString\",\"text\":"+quote(s)+"}");++texts_;
            }
            return quote(s);
        }
        if(d.type=="Utf8StrProperty"){
            std::string s;if(!utf8String(address,s))return unavailable("invalid FUtf8String",address,d.size);
            return quote(s);
        }
        if(d.type=="TextProperty")return text(address,d);
        if(d.type=="StructProperty"&&d.target)return structure(address,*d.target,depth+1);
        if(d.type=="ObjectProperty"||d.type=="ClassProperty"||d.type=="ObjectPtrProperty"){
            std::uint64_t p=0;if(!memory_.get(address,p))return unavailable("unreadable object reference",address,d.size);
            if(!p)return "null";
            auto i=objects_.find(p);
            if(i==objects_.end()||!current(*i->second))return unavailable("object reference absent/stale/encoded",address,d.size);
            line(references_,"{\"source\":"+quote(location_)+",\"kind\":\"object\",\"target\":"+quote(i->second->path)+",\"index\":"+std::to_string(i->second->index)+"}");
            assetReference(i->second->path,"object");
            return "{\"object\":"+quote(i->second->path)+",\"index\":"+std::to_string(i->second->index)+"}";
        }
        if(d.type=="SoftObjectProperty"||d.type=="SoftClassProperty"){
            auto i=paths_.find("/Script/CoreUObject.SoftObjectPath");
            if(i!=paths_.end()&&i->second->size&&*i->second->size<=d.size&&!i->second->fields.empty()){
                const auto pathAddress=address+d.size-*i->second->size;
                const auto path=structure(pathAddress,*i->second,depth+1);
                const auto canonical=assetPath(pathAddress,*i->second);
                if(canonical.empty())return unavailable("soft asset path could not be decoded",pathAddress,*i->second->size);
                line(references_,"{\"source\":"+quote(location_)+",\"kind\":\"soft_path\",\"target\":"+quote(canonical)+",\"value\":"+path+"}");
                if(canonical!="None")assetReference(canonical,"soft_path");
                return "{\"asset_path\":"+quote(canonical)+",\"value\":"+path+"}";
            }
        }
        if(d.type=="ArrayProperty"||d.type=="MapProperty"||d.type=="SetProperty")return container(address,d,depth);
        return unavailable("unsupported/unverified property: "+d.type,address,d.size);
    }
    void index(const Object& object,const std::string& identity,const std::string& file,const std::string& kind,std::size_t count,std::size_t issues){
        checkpoint();
        const auto groups=categories(identity);
        std::string cats="[";for(const auto& c:groups){if(cats.size()>1)cats+=',';cats+=quote(c);}cats+=']';
        const auto recordFor=[&](const std::string& payload,const std::string& source){
            return "{\"object\":"+quote(object.path)+",\"class\":"+quote(object.className)+",\"kind\":"+quote(kind)+
                ",\"file\":"+quote(payload)+(source.empty()?"":",\"source_file\":"+quote(source))+
                ",\"records\":"+std::to_string(count)+",\"unresolved\":"+std::to_string(issues)+",\"categories\":"+cats+"}";
        };
        const auto record=recordFor(file,"");
        const auto payloadBytes=std::filesystem::file_size(output_/file);
        std::uint64_t required=record.size()+1;
        for(const auto& c:groups)required+=payloadBytes+recordFor(c+"/"+file,file).size()+1;
        if(required>maxBytes_-bytes_)throw std::runtime_error("resource category payload byte budget exceeded");
        line(manifest_,record);
        // Category indexes remain metadata-only. Publish independent body files
        // from the completed disk payload, never reread the moving game object.
        for(const auto& c:groups){
            checkpoint();
            const auto categoryFile=c+"/"+file;
            std::filesystem::create_directories((output_/categoryFile).parent_path());
            std::filesystem::copy_file(output_/file,output_/categoryFile);
            bytes_+=payloadBytes;
            line(indexes_.at(c),recordFor(categoryFile,file));++counts_[c];
        }
        checkpoint();
        ++resources_;
        capturedBodies_[object.path]={file,kind,count,issues};
    }
    void table(const Object& o,const Type& type){
        const Property* rowProperty=nullptr;
        const Type* base=&type;
        for(unsigned n=0;base&&n<64;++n){
            for(const auto& p:base->fields)if(p.name=="RowStruct")rowProperty=&p;
            auto i=paths_.find(base->super);base=i==paths_.end()?nullptr:i->second;
        }
        std::uint64_t rowTypeAddress=0;
        if(!rowProperty||!memory_.get(o.address+rowProperty->offset,rowTypeAddress)||!types_.count(rowTypeAddress)){
            unavailable("DataTable RowStruct is absent: "+o.path,o.address);return;
        }
        const auto& rowType=*types_.at(rowTypeAddress);
        if(rowType.kind!="struct"||!rowType.size||*rowType.size<=0){unavailable("invalid table row schema: "+o.path,o.address);return;}
        // The 80-byte native RowMap fits the gap immediately after RowStruct.
        // A profile override remains version-bound to the validated executable.
        const auto offset=config_.number("resource_datatable_rowmap_offset",rowProperty->offset+8);
        if(offset<0||!type.size||offset+80>*type.size){unavailable("DataTable RowMap outside class",o.address);return;}
        SparseView view;
        if(!sparse(o.address+offset,view)){unavailable("DataTable RowMap layout not verified: "+o.path,o.address+offset,80);return;}
        std::vector<std::pair<std::string,std::uint64_t>> rows;
        std::set<std::string> names;
        for(auto slot:view.live){
            checkpoint();const auto at=view.header.data+std::uint64_t(slot)*24;
            auto name=reflection_.name(at,0);std::uint64_t data=0;unsigned char probe=0;
            if(name.empty()||!names.insert(name).second||!memory_.get(at+8,data)||!data||!memory_.get(data,probe)){
                unavailable("DataTable row map entry failed validation: "+o.path,at,16);return;
            }
            rows.emplace_back(name,data);
        }
        const auto file=prepareBodyPath(o.path);
        std::ofstream out(output_/file,std::ios::binary);if(!out)throw std::runtime_error("cannot open table output");
        const auto before=unresolved_;
        line(out,"{\"kind\":\"DataTable\",\"object\":"+quote(o.path)+",\"row_type\":"+quote(rowType.path)+",\"row_map_offset\":"+std::to_string(offset)+",\"layout\":\"validated_native_candidate\"}");
        const bool miniMapFogBlock=rowType.path=="/Script/HTGame.MiniMapFogBlockData";
        for(const auto& row:rows){
            location_=o.path+"["+row.first+"]";
            line(out,"{\"row\":"+quote(row.first)+(miniMapFogBlock?",\"map_block_id\":"+quote(row.first):std::string())+
                ",\"values\":"+structure(row.second,rowType,0)+"}");
            ++rows_;
        }
        SparseView after;
        if(!current(o)||!sparse(o.address+offset,after)||!(view.header==after.header)||view.live!=after.live)
            unavailable("DataTable changed during capture: "+o.path,o.address);
        out.close();if(!out)throw std::runtime_error("cannot finalize table output");
        index(o,o.path+" "+rowType.path,file,"DataTable",rows.size(),unresolved_-before);
    }
    void stringTable(const Object& o){
        const auto before=unresolved_;
        std::uint64_t native=0;
        // UObject's native FStringTable shared reference follows its 40-byte base.
        if(!memory_.get(o.address+40,native)||native<0x10000||(native&7)){
            unavailable("StringTable native reference unavailable: "+o.path,o.address,64);return;
        }
        struct Candidate {unsigned offset;SparseView view;std::vector<std::pair<std::string,std::uint64_t>> entries;};
        std::vector<Candidate> candidates;
        for(unsigned offset=0;offset+80<=512;offset+=8){
            checkpoint();Candidate c{offset,{},{}};
            if(!sparse(native+offset,c.view)||c.view.live.empty())continue;
            bool valid=true;std::set<std::string> keys;std::uint64_t keyBytes=0;
            for(auto n:c.view.live){
                checkpoint();
                const auto at=c.view.header.data+std::uint64_t(n)*40;
                std::string key;std::uint64_t entry=0;
                if(!string(at,key)||key.empty()||!keys.insert(key).second||!memory_.get(at+16,entry)||entry<0x10000||(entry&7)){valid=false;break;}
                keyBytes+=key.size()*2;
                if(keyBytes>maxBytes_-bytes_)throw std::runtime_error("StringTable candidate key byte budget exceeded");
                c.entries.emplace_back(key,entry);
            }
            if(valid)candidates.push_back(std::move(c));
        }
        if(candidates.size()!=1){unavailable("StringTable requires unique validated key map: "+o.path,native);return;}
        const auto& c=candidates.front();
        std::vector<unsigned> sourceOffsets;
        for(unsigned off=0;off+16<=128;off+=8){
            bool valid=true;std::size_t nonempty=0;
            for(const auto& entry:c.entries){checkpoint();std::string s;if(!string(entry.second+off,s)){valid=false;break;}if(!s.empty())++nonempty;}
            if(valid&&nonempty)sourceOffsets.push_back(off);
        }
        const auto file=prepareBodyPath(o.path);
        std::ofstream out(output_/file,std::ios::binary);if(!out)throw std::runtime_error("cannot open text output");
        line(out,"{\"kind\":\"StringTable\",\"object\":"+quote(o.path)+",\"layout\":\"inferred_candidates\",\"map_offset\":"+std::to_string(c.offset)+",\"translations_verified\":false}");
        for(const auto& entry:c.entries){
            std::string values="[";
            for(auto off:sourceOffsets){std::string s;if(!string(entry.second+off,s))continue;if(values.size()>1)values+=',';values+="{\"offset\":"+std::to_string(off)+",\"text\":"+quote(s)+"}";bufferBudget(values);}
            line(out,"{\"key\":"+quote(entry.first)+",\"source_candidates\":"+values+"]}");++texts_;
        }
        unavailable("StringTable native source/display semantics require runtime confirmation: "+o.path,native);
        SparseView after;
        if(!current(o)||!sparse(native+c.offset,after)||!(c.view.header==after.header)||c.view.live!=after.live)
            unavailable("StringTable changed during capture: "+o.path,o.address);
        out.close();if(!out)throw std::runtime_error("cannot finalize text output");
        index(o,"localization "+o.path,file,"StringTable",c.entries.size(),unresolved_-before);
    }
    std::string extraKind(const Object& o,const Type& type)const{
        // Type definitions themselves are metadata, not instances to serialize.
        if(types_.count(o.address)||type.kind!="class")return {};
        if(isA(&type,"/Script/Engine.World"))return "RuntimeWorld";
        if(isA(&type,"/Script/Engine.Level"))return "RuntimeLevel";
        if(isA(&type,"/Script/Engine.LevelStreaming"))return "LevelStreaming";
        if(isA(&type,"/Script/Engine.Actor"))return "RuntimeActor";
        if(isA(&type,"/Script/Engine.ActorComponent"))return "RuntimeComponent";
        if(scriptIdentity(type.path))return "ScriptAsset";
        if(isA(&type,"/Script/Engine.CurveTable"))return "CurveTableMetadata";
        for(const auto* base:{"/Script/Engine.CurveBase","/Script/Engine.Blueprint",
                              "/Script/AIModule.BehaviorTree","/Script/AIModule.BlackboardData",
                              "/Script/DeveloperSettings.DeveloperSettings"})
            if(isA(&type,base))return "GameplayAsset";
        // Preserve loaded game CDOs, including configuration UObjects that do
        // not inherit DataAsset. Never follow references or execute Lua here.
        if(o.name.rfind("Default__",0)==0&&
           (type.path.rfind("/Game/",0)==0||type.path.rfind("/Script/HTGame.",0)==0))return "ClassDefaults";
        if(config_.number("resource_game_objects",1)&&
           (type.path.rfind("/Game/",0)==0||type.path.rfind("/Script/HTGame.",0)==0))return "GameplayObject";
        return {};
    }
    void objectValues(const Object& o,const Type& type,const std::string& kind){
        if(!current(o)){unavailable("stale resource object: "+o.path,o.address);return;}
        const auto before=unresolved_;const auto file=prepareBodyPath(o.path);
        location_=o.path;
        std::uint64_t outer=0;reflection_.read(o.address,reflection_.layout().objOuter,outer);
        auto parent=objects_.find(outer);
        const auto outerPath=parent!=objects_.end()&&current(*parent->second)?quote(parent->second->path):"null";
        std::ofstream out(output_/file,std::ios::binary);if(!out)throw std::runtime_error("cannot open object output: "+(output_/file).u8string());
        if(kind=="CurveTableMetadata")unavailable("CurveTable native curve rows are not decoded: "+o.path,o.address);
        line(out,"{\"kind\":"+quote(kind)+",\"object\":"+quote(o.path)+",\"class\":"+quote(type.path)+
            ",\"outer\":"+outerPath+",\"is_default\":"+(o.name.rfind("Default__",0)==0?"true":"false")+
            ",\"coverage\":\"loaded_reflected_values_only\",\"values\":"+structure(o.address,type,0)+"}");
        if(!current(o))unavailable("resource object changed during capture: "+o.path,o.address);
        std::string identity=o.path+" "+type.path+" "+kind;
        for(const auto& p:type.fields)identity+=" "+p.name;
        out.close();if(!out)throw std::runtime_error("cannot finalize object output");
        index(o,identity,file,kind,1,unresolved_-before);
    }
    // Targeted capture helpers are defined here, using the same validated
    // value decoder and budgets as ordinary resource bodies.
    #include "ebon_feature_capture.inl"
    void state(const std::string& status,const std::string& error=""){
        // Flush at publication boundaries; per-row flushing made large tables
        // and reference reports issue a disk write for every JSON record.
        for(auto* stream:{&manifest_,&issues_,&textValues_,&references_}){
            if(stream->is_open()){stream->flush();if(!*stream)throw std::runtime_error("cannot flush resource output");}
        }
        for(auto& index:indexes_){index.second.flush();if(!index.second)throw std::runtime_error("cannot flush category index");}
        std::string categoriesJson="{";
        for(const auto& c:categoryNames()){
            if(categoriesJson.size()>1)categoriesJson+=',';
            const bool seen=counts_[c]||(c=="localization"&&texts_);
            categoriesJson+=quote(c)+":{\"resources\":"+std::to_string(counts_[c])+",\"coverage\":"+quote(seen?"observed_partial":"not_observed")+"}";
        }
        const bool ok=error.empty()&&resources_>0&&(status=="complete"||status=="complete_with_unresolved");
        save(output_/"resource_state.json","{\"format\":\"EbonResources-v1\",\"status\":"+quote(status)+",\"success\":"+(ok?"true":"false")+
            ",\"build_sha256\":"+quote(reflection_.moduleSha256)+",\"resources\":"+std::to_string(resources_)+",\"rows\":"+std::to_string(rows_)+
            ",\"text_entries\":"+std::to_string(texts_)+",\"unresolved\":"+std::to_string(unresolved_)+",\"values\":"+std::to_string(values_)+
            ",\"bytes\":"+std::to_string(bytes_)+",\"categories\":"+categoriesJson+"},\"error\":"+(error.empty()?"null":quote(error))+
            ",\"category_payloads\":\"separate_files\",\"file_paths\":\"relative_to_capture_root\",\"body_naming\":\"object_path\",\"coverage\":\"loaded tables, assets, world/level/actor/component reflected values, game class defaults and text candidates; unloaded cells, cooked map reconstruction, script VM contents and complete translations are not established\"}\n");
    }
public:
    Dumper(const MemoryReader& memory,Reflection& reflection,const Config& config,std::filesystem::path output)
        :memory_(memory),reflection_(reflection),config_(config),output_(std::move(output)){
        maxEntries_=(std::int32_t)option("resource_max_entries",100000,1,1000000);
        maxString_=(std::int32_t)option("resource_max_string",65536,1,1048576);
        maxDepth_=(unsigned)option("resource_max_depth",16,1,64);
        maxValues_=(std::uint64_t)option("resource_max_values",2000000,1,100000000);
        maxBytes_=(std::uint64_t)option("resource_max_output_mb",256,1,4096)*1024*1024;
        deadline_=GetTickCount64()+(ULONGLONG)option("resource_timeout_seconds",180,1,1800)*1000;
        for(const auto& t:reflection.types){types_[t.address]=&t;paths_[t.path]=&t;}
        for(const auto& o:reflection.objects)objects_[o.address]=&o;
    }
    // UE5 default TSparseArray: array(16), inline-4 bit allocator(24),
    // NumBits/MaxBits(8), FirstFreeIndex/NumFreeIndices(8).
    bool sparse(std::uint64_t address,SparseView& view)const{
        view={};if(!header(address,view.header))return false;
        std::int32_t bits=0,maxBits=0,free=0,first=0;
        if(!memory_.get(address+40,bits)||!memory_.get(address+44,maxBits)||!memory_.get(address+48,first)||!memory_.get(address+52,free)||
           bits!=view.header.num||maxBits<bits||maxBits>maxEntries_*16||free<0||free>bits||
           (free?(first<0||first>=bits):first!=-1))return false;
        if(!bits)return true;
        std::uint64_t storage=address+16;
        if(maxBits>128&&(!memory_.get(address+32,storage)||storage<0x10000))return false;
        std::vector<std::uint32_t> flags((bits+31)/32);
        if(!memory_.copy(storage,flags.data(),flags.size()*4))return false;
        if(free&&(flags[first/32]&(1u<<(first%32))))return false;
        for(int i=0;i<bits;++i)if(flags[i/32]&(1u<<(i%32)))view.live.push_back(i);
        return view.live.size()==std::size_t(bits-free);
    }
    bool run(bool initializingStateOwned=false,const std::function<void()>& afterResources={}){
        std::filesystem::create_directories(output_/"data");
        if((std::filesystem::exists(output_/"resource_state.json")&&!initializingStateOwned)||
           std::filesystem::exists(output_/"resource_manifest.jsonl")||
           std::filesystem::exists(output_/"unresolved.jsonl")||
           std::filesystem::exists(output_/"references.jsonl")||!std::filesystem::is_empty(output_/"data"))
            throw std::runtime_error("resource output already used");
        manifest_.open(output_/"resource_manifest.jsonl",std::ios::binary);
        issues_.open(output_/"unresolved.jsonl",std::ios::binary);
        references_.open(output_/"references.jsonl",std::ios::binary);
        if(!manifest_||!issues_||!references_)throw std::runtime_error("cannot open resource manifest");
        for(const auto& c:categoryNames()){
            std::filesystem::create_directories(output_/c);
            indexes_[c].open(output_/c/"resources.jsonl",std::ios::binary);
            if(!indexes_[c])throw std::runtime_error("cannot open category index");
        }
        textValues_.open(output_/"localization"/"text_values.jsonl",std::ios::binary);
        if(!textValues_)throw std::runtime_error("cannot open localization text output");
        state("collecting");
        try{
            location_="reflection_inventory";
            const auto diagnosticCount=reflection_.errors.size()+reflection_.unresolved.size()+reflection_.optionalUnresolved.size();
            unresolved_+=diagnosticCount;
            std::size_t diagnosticRows=0;
            const auto addDiagnostics=[&](const std::vector<std::string>& diagnostics,const char* kind){
                for(const auto& diagnostic:diagnostics){
                    if(diagnosticRows>=1000)break;
                    line(issues_,"{\"kind\":"+quote(kind)+",\"reason\":"+quote(diagnostic)+"}");
                    ++diagnosticRows;
                }
            };
            addDiagnostics(reflection_.errors,"reflection_error");
            if(diagnosticRows<1000)addDiagnostics(reflection_.unresolved,"reflection_unresolved");
            if(diagnosticRows<1000)addDiagnostics(reflection_.optionalUnresolved,"reflection_optional_unresolved");
            capturePriorities();
            if(config_.number("resource_feature_only",0)){
                if(afterResources)afterResources();
                state(resources_?(unresolved_?"complete_with_unresolved":"complete"):"no_resources");
                return resources_>0;
            }
            for(const auto& o:reflection_.objects){
                if(capturedBodies_.count(o.path)||types_.count(o.address))continue;
                checkpoint();auto i=types_.find(o.klass);if(i==types_.end())continue;
                const auto& type=*i->second;
                if(isA(&type,"/Script/Engine.DataTable")&&o.name.rfind("Default__",0)!=0){
                    if(current(o))table(o,type);else unavailable("stale DataTable: "+o.path,o.address);
                }else if(isA(&type,"/Script/Engine.StringTable")&&o.name.rfind("Default__",0)!=0){
                    if(current(o))stringTable(o);else unavailable("stale StringTable: "+o.path,o.address);
                }else if(isA(&type,"/Script/Engine.DataAsset")){
                    if(!current(o)){unavailable("stale DataAsset: "+o.path,o.address);continue;}
                    const auto before=unresolved_;const auto file=prepareBodyPath(o.path);
                    location_=o.path;
                    std::ofstream out(output_/file,std::ios::binary);if(!out)throw std::runtime_error("cannot open asset output");
                    line(out,"{\"kind\":\"DataAsset\",\"object\":"+quote(o.path)+",\"class\":"+quote(type.path)+
                        ",\"is_default\":"+(o.name.rfind("Default__",0)==0?"true":"false")+",\"values\":"+structure(o.address,type,0)+"}");
                    if(!current(o))unavailable("DataAsset changed during capture: "+o.path,o.address);
                    // Class field names contribute only to routing, not payload values.
                    std::string identity=o.path+" "+type.path;
                    for(const auto& p:type.fields)identity+=" "+p.name;
                    out.close();if(!out)throw std::runtime_error("cannot finalize asset output");
                    index(o,identity,file,"DataAsset",1,unresolved_-before);
                }
            }
            // Tables and DataAssets retain priority over potentially numerous
            // scene objects. Each object is emitted once and shares the budget.
            for(const auto& o:reflection_.objects){
                if(capturedBodies_.count(o.path)||types_.count(o.address))continue;
                checkpoint();auto i=types_.find(o.klass);if(i==types_.end())continue;
                const auto& type=*i->second;
                if(isA(&type,"/Script/Engine.DataTable")||isA(&type,"/Script/Engine.StringTable")||
                   isA(&type,"/Script/Engine.DataAsset"))continue;
                const auto kind=extraKind(o,type);
                if(!kind.empty())objectValues(o,type,kind);
            }
            if(afterResources)afterResources();
            state(resources_?(unresolved_?"complete_with_unresolved":"complete"):"no_resources");
            return resources_>0;
        }catch(const std::exception& e){state("failed",e.what());return false;}
    }
};
} // namespace ebon::resources
