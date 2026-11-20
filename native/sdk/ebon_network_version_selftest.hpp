#pragma once
#include "ebon_network_version.hpp"

template<class Memory> bool networkVersionSelfTest(Memory& memory,std::ostream* output=nullptr){
    constexpr std::uint64_t base=0x10000,getter=0x10880,hello=0x10980,flag=0x18000;
    const std::string digest(64,'a');
    ebon::Config config;config.values={{"exe_sha256",digest},{"network_version_getter_rva","0x880"},
                                      {"network_version_hello_call_rva","0x980"}};
    const ebon::Module module{base,0x10000,L"synthetic-network-version.exe",L"synthetic"};
    const unsigned char prefix[]={0x48,0x83,0xec,0x78,0x80,0x3d,0,0,0,0,0,0x74,0x0b,0x8b,0x05,0,0,0,0,0x48,0x83,0xc4,0x78,0xc3};
    const unsigned char call[]={0xe8,0,0,0,0,0x89,0x44,0x24,0x28};
    memory.putBytes(getter,prefix,sizeof(prefix));memory.putBytes(hello,call,sizeof(call));
    memory.put(getter+6,(std::int32_t)(flag-getter-11));
    memory.put(getter+15,(std::int32_t)(flag+4-getter-19));
    memory.put(hello+1,(std::int32_t)((std::int64_t)getter-(std::int64_t)hello-5));
    memory.put(flag,std::uint8_t(0));memory.put(flag+4,std::uint32_t(123));
    auto result=ebon::observeNetworkVersion(memory,config,module,digest);
    if(result.value||result.status!="not_initialized")return false;
    memory.put(flag,std::uint8_t(1));memory.put(flag+4,std::uint32_t(0));
    result=ebon::observeNetworkVersion(memory,config,module,digest);
    if(!result.value||*result.value!=0||result.status!="observed_cached")return false;
    memory.put(flag+4,std::uint32_t(0xffffffff));
    result=ebon::observeNetworkVersion(memory,config,module,digest);
    if(!result.value||*result.value!=0xffffffff||result.cacheRva!=0x8004||result.flagRva!=0x8000)return false;
    auto wrong=config;wrong.values["exe_sha256"]=std::string(64,'b');
    if(ebon::observeNetworkVersion(memory,wrong,module,digest).status!="profile_mismatch")return false;
    wrong=config;wrong.values["network_version_collect"]="0";
    if(ebon::observeNetworkVersion(memory,wrong,module,digest).value)return false;
    memory.put(getter+4,std::uint8_t(0x90));
    if(ebon::observeNetworkVersion(memory,config,module,digest).status!="signature_mismatch")return false;
    memory.put(getter+4,std::uint8_t(0x80));memory.put(hello+1,std::int32_t(0));
    if(ebon::observeNetworkVersion(memory,config,module,digest).status!="invalid_binding")return false;
    memory.put(hello+1,(std::int32_t)((std::int64_t)getter-(std::int64_t)hello-5));
    memory.put(flag,std::uint8_t(2));
    if(ebon::observeNetworkVersion(memory,config,module,digest).status!="invalid_cache")return false;
    memory.put(flag,std::uint8_t(1));
    struct Changing final:ebon::MemoryReader {
        Memory& source;mutable bool changed=false;
        explicit Changing(Memory& value):source(value){}
        bool copy(std::uint64_t at,void* out,std::size_t size)const override{
            const auto ok=source.copy(at,out,size);
            if(at==0x18000&&!changed){source.put(at+4,std::uint32_t(55));changed=true;}return ok;
        }
        bool executable(std::uint64_t at)const override{return source.executable(at);}
        std::vector<ebon::Region> regions(const ebon::Module& mod)const override{return source.regions(mod);}
    } changing(memory);
    if(ebon::observeNetworkVersion(changing,config,module,digest).status!="changed_during_read")return false;
    memory.put(flag+4,std::uint32_t(0xffffffff));
    result=ebon::observeNetworkVersion(memory,config,module,digest);
    if(output)*output<<result.json();
    return result.value.has_value();
}
