#pragma once
#include "../common/ebon_common.hpp"
#include <optional>

namespace ebon {

inline std::string networkVersionHex(const unsigned char* data,std::size_t size){
    static const char digits[]="0123456789abcdef";std::string text;text.reserve(size*2);
    for(std::size_t i=0;i<size;++i){text+=digits[data[i]>>4];text+=digits[data[i]&15];}return text;
}

struct NetworkVersionObservation {
    std::string status="unsupported_profile",error,sha256,module;
    std::uint64_t moduleBase=0,moduleSize=0,getterRva=0,helloRva=0,flagRva=0,cacheRva=0;
    std::array<unsigned char,24> getter{};
    std::array<unsigned char,9> hello{};
    std::array<unsigned char,8> cache{};
    std::array<unsigned char,8> cacheAfter{};
    std::optional<std::uint32_t> value;
    std::string json()const{
        return "{\"format\":\"EbonNetworkVersion-v1\",\"status\":"+quote(status)+
            ",\"verified\":"+(value?"true":"false")+",\"value\":"+(value?std::to_string(*value):"null")+
            ",\"build_sha256\":"+quote(sha256)+",\"module\":"+quote(module)+
            ",\"module_base\":"+quote(hex(moduleBase))+",\"module_size\":"+std::to_string(moduleSize)+
            ",\"getter_rva\":"+quote(hex(getterRva))+",\"hello_call_rva\":"+quote(hex(helloRva))+
            ",\"cache_flag_rva\":"+quote(hex(flagRva))+",\"cache_value_rva\":"+quote(hex(cacheRva))+
            ",\"getter_bytes\":"+quote(networkVersionHex(getter.data(),getter.size()))+
            ",\"hello_call_bytes\":"+quote(networkVersionHex(hello.data(),hello.size()))+
            ",\"cache_bytes\":"+quote(networkVersionHex(cache.data(),cache.size()))+
            ",\"cache_bytes_after\":"+quote(networkVersionHex(cacheAfter.data(),cacheAfter.size()))+
            ",\"cache_read_twice\":"+(value?"true":"false")+
            ",\"source\":\"validated GetLocalNetworkVersion cache and Hello callsite; no engine calls\""+
            ",\"packet_mapping_verified\":false,\"error\":"+(error.empty()?"null":quote(error))+"}\n";
    }
};

inline NetworkVersionObservation observeNetworkVersion(const MemoryReader& memory,const Config& config,
                                                       const Module& module,const std::string& digest){
    NetworkVersionObservation result;result.sha256=digest;result.module=utf8(module.name);
    result.moduleBase=module.base;result.moduleSize=module.size;
    const auto fail=[&](const char* state,const char* error){result.status=state;result.error=error;result.value.reset();return result;};
    if(!config.number("network_version_collect",1))return fail("disabled","network version capture disabled");
    const auto getter=config.number("network_version_getter_rva",0),hello=config.number("network_version_hello_call_rva",0);
    if(getter<=0||hello<=0)return fail("unsupported_profile","profile has no verified getter/Hello binding");
    auto expected=config.text("exe_sha256");
    std::transform(expected.begin(),expected.end(),expected.begin(),[](unsigned char c){return (char)std::tolower(c);});
    if(expected.size()!=64||digest!=expected)return fail("profile_mismatch","full module SHA-256 binding is required");
    result.getterRva=(std::uint64_t)getter;result.helloRva=(std::uint64_t)hello;
    const auto extent=[&](std::uint64_t rva,std::size_t size){return module.base>=0x10000&&
        module.base+module.size>=module.base&&rva<module.size&&size<=module.size-rva;};
    if(!extent(result.getterRva,result.getter.size())||!extent(result.helloRva,result.hello.size()))
        return fail("invalid_binding","code RVA outside module");
    if(!memory.executable(module.base+result.getterRva)||!memory.executable(module.base+result.helloRva)||
       !memory.copy(module.base+result.getterRva,result.getter.data(),result.getter.size())||
       !memory.copy(module.base+result.helloRva,result.hello.data(),result.hello.size()))
        return fail("unreadable","version code is not readable executable memory");
    const auto& g=result.getter;const auto& h=result.hello;
    // x64: sub rsp,N; cmp byte [rip+flag],0; je cold; mov eax,[rip+value]; add rsp,N; ret.
    if(g[0]!=0x48||g[1]!=0x83||g[2]!=0xec||g[4]!=0x80||g[5]!=0x3d||g[10]!=0||
       g[11]!=0x74||g[12]!=0x0b||g[13]!=0x8b||g[14]!=0x05||
       g[19]!=0x48||g[20]!=0x83||g[21]!=0xc4||g[22]!=g[3]||g[23]!=0xc3||
       h[0]!=0xe8||h[5]!=0x89||h[6]!=0x44||h[7]!=0x24||h[8]!=0x28)
        return fail("signature_mismatch","getter cached branch or Hello EAX destination differs");
    std::int32_t flagDisplacement=0,cacheDisplacement=0,callDisplacement=0;
    std::memcpy(&flagDisplacement,g.data()+6,4);std::memcpy(&cacheDisplacement,g.data()+15,4);
    std::memcpy(&callDisplacement,h.data()+1,4);
    const auto flag=(std::int64_t)result.getterRva+11+flagDisplacement;
    const auto value=(std::int64_t)result.getterRva+19+cacheDisplacement;
    const auto callee=(std::int64_t)result.helloRva+5+callDisplacement;
    if(flag<0||value!=flag+4||callee!=(std::int64_t)result.getterRva||
       !extent((std::uint64_t)flag,result.cache.size())||((std::uint64_t)value&3))
        return fail("invalid_binding","RIP cache extents or Hello callee failed validation");
    result.flagRva=(std::uint64_t)flag;result.cacheRva=(std::uint64_t)value;
    if(memory.executable(module.base+result.flagRva)||
       !memory.copy(module.base+result.flagRva,result.cache.data(),result.cache.size())||
       !memory.copy(module.base+result.flagRva,result.cacheAfter.data(),result.cacheAfter.size()))
        return fail("unreadable","version cache is not readable non-executable data");
    if(result.cache!=result.cacheAfter)return fail("changed_during_read","version cache changed between observations");
    if(result.cache[0]==0)return fail("not_initialized","engine has not initialized the cached checksum; no forced evaluation");
    if(result.cache[0]!=1)return fail("invalid_cache","invalid cached checksum flag");
    std::uint32_t checksum=0;std::memcpy(&checksum,result.cache.data()+4,4);
    result.value=checksum;result.status="observed_cached";return result;
}

// Bounded atomic publication; readers may temporarily deny Windows delete sharing.
inline void saveNetworkVersion(const std::filesystem::path& output,const NetworkVersionObservation& value){
    std::filesystem::create_directories(output);
    const auto destination=output/"network_version.json";
    const auto temporary=output/("network_version."+std::to_string(GetCurrentProcessId())+"."+std::to_string(GetCurrentThreadId())+".tmp");
    save(temporary,value.json());
    for(unsigned attempt=0;attempt<50;++attempt){
        if(MoveFileExW(temporary.c_str(),destination.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))return;
        const auto error=GetLastError();
        if(error!=ERROR_SHARING_VIOLATION&&error!=ERROR_ACCESS_DENIED)throw std::runtime_error("cannot publish network version state");
        Sleep(10);
    }
    throw std::runtime_error("network version state publication remained locked");
}
} // namespace ebon
