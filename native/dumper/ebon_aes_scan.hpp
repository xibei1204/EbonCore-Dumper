#pragma once
#include "ebon_aes_hunter.hpp"
#include <psapi.h>

namespace ebon_hunter {
namespace fs = std::filesystem;
constexpr std::size_t maxIndexBytes = 256 * 1024 * 1024;
constexpr std::size_t chunkBytes = 1024 * 1024;
constexpr std::size_t overlapBytes = 255; // Includes a complete immediate-key construction window.

template<class T> inline T load(const uint8_t* p) { T v; std::memcpy(&v,p,sizeof(v)); return v; }
inline std::string hexBytes(const uint8_t* p, std::size_t n) {
    static const char digits[]="0123456789ABCDEF";
    std::string result; result.reserve(n*2);
    for(std::size_t i=0;i<n;++i){result+=digits[p[i]>>4];result+=digits[p[i]&15];}
    return result;
}
inline bool hasAesNi() { int regs[4]{}; __cpuid(regs,1); return (regs[2] & (1<<25)) != 0; }

struct PakSample {
    fs::path path;
    uint32_t version=0;
    uint64_t indexOffset=0;
    uint64_t serializedIndexOffset=0;
    int offsetAdjustment=0;
    std::array<uint8_t,20> digest{};
    std::array<uint8_t,16> guid{};
    std::vector<uint8_t> cipher;
    std::vector<uint8_t> nteCipher;
    bool ioStore=false;
    uint32_t chunkCount=0;
};

inline PakSample loadPak(const fs::path& path) {
    std::ifstream f(path,std::ios::binary);
    if(!f)throw std::runtime_error("cannot open Pak: "+path.u8string());
    f.seekg(0,std::ios::end); const auto end=f.tellg();
    // Standard Pak v8 and v10-v12 footer: GUID + encrypted flag + 204 bytes.
    if(end<221)throw std::runtime_error("truncated Pak footer: "+path.u8string());
    const auto size=static_cast<uint64_t>(end);
    uint8_t footer[221]{};
    f.seekg(static_cast<std::streamoff>(size-221));
    if(!f.read(reinterpret_cast<char*>(footer),sizeof(footer)))throw std::runtime_error("cannot read Pak footer");
    const auto magic=load<uint32_t>(footer+17),version=load<uint32_t>(footer+21);
    if(magic!=0x5A6F12E1 || (version!=8&&version!=10&&version!=11&&version!=12))
        throw std::runtime_error("unsupported standard Pak footer: "+path.u8string());
    if(footer[16]!=1)throw std::runtime_error("Pak does not have an encrypted index: "+path.u8string());
    const auto offset=load<uint64_t>(footer+25),length=load<uint64_t>(footer+33);
    const auto inRange=[&](uint64_t at){return at<=size-221&&length<=size-221-at;};
    const bool standard=inRange(offset),nte=version==12&&offset&&inRange(offset-1);
    if(length<16 || length>maxIndexBytes || length%16 || (!standard&&!nte))
        throw std::runtime_error("invalid encrypted Pak index range: "+path.u8string());
    PakSample sample; sample.path=path; sample.version=version;sample.serializedIndexOffset=offset;
    sample.indexOffset=standard?offset:offset-1;sample.offsetAdjustment=standard?0:-1;
    std::memcpy(sample.digest.data(),footer+41,20); std::memcpy(sample.guid.data(),footer,16);
    sample.cipher.resize(static_cast<std::size_t>(length));
    f.seekg(static_cast<std::streamoff>(sample.indexOffset));
    if(!f.read(reinterpret_cast<char*>(sample.cipher.data()),static_cast<std::streamsize>(length)))
        throw std::runtime_error("truncated encrypted Pak index");
    if(standard&&nte) {
        // NTE v12 serializes IndexOffset+1. Only a complete SHA-1 match chooses
        // this alternative; standard v12 remains the first verification path.
        sample.nteCipher.resize(static_cast<std::size_t>(length));f.seekg(static_cast<std::streamoff>(offset-1));
        if(!f.read(reinterpret_cast<char*>(sample.nteCipher.data()),static_cast<std::streamsize>(length)))
            throw std::runtime_error("truncated NTE encrypted Pak index");
    }
    return sample;
}

inline PakSample loadUtoc(const fs::path& path) {
    std::ifstream f(path,std::ios::binary);
    if(!f)throw std::runtime_error("cannot open IoStore TOC: "+path.u8string());
    f.seekg(0,std::ios::end);const auto end=f.tellg();
    if(end<144)throw std::runtime_error("truncated IoStore header");
    const auto size=static_cast<uint64_t>(end);
    uint8_t header[144]{};f.seekg(0);
    if(!f.read(reinterpret_cast<char*>(header),144))throw std::runtime_error("cannot read IoStore header");
    if(std::memcmp(header,"-==--==--==--==-",16)||header[16]<3||header[16]>8||load<uint32_t>(header+20)!=144)
        throw std::runtime_error("unsupported IoStore header: "+path.u8string());
    const auto flags=header[80];
    const auto count=load<uint32_t>(header+24),blocks=load<uint32_t>(header+28);
    const auto methods=load<uint32_t>(header+36),methodLength=load<uint32_t>(header+40);
    const auto length=load<uint32_t>(header+48);
    if((flags&~31)||!(flags&2)||!(flags&8)||!count||length<16||length%16||length>maxIndexBytes)
        throw std::runtime_error("IoStore TOC has no supported encrypted directory index: "+path.u8string());
    if(count>2000000||blocks>2000000||load<uint32_t>(header+32)!=12||methods>64||!methodLength||methodLength>256)
        throw std::runtime_error("invalid IoStore table sizes");
    uint64_t offset=144+static_cast<uint64_t>(count)*22+static_cast<uint64_t>(blocks)*12+
                    static_cast<uint64_t>(methods)*methodLength;
    if(header[16]>=4)offset+=static_cast<uint64_t>(load<uint32_t>(header+84))*4;
    if(header[16]>=5)offset+=static_cast<uint64_t>(load<uint32_t>(header+96))*4;
    if(flags&4) {
        if(offset>size||size-offset<4)throw std::runtime_error("truncated IoStore signatures");
        uint8_t field[4];f.seekg(static_cast<std::streamoff>(offset));
        if(!f.read(reinterpret_cast<char*>(field),4))throw std::runtime_error("cannot read IoStore signature size");
        const auto signature=load<int32_t>(field);
        if(signature<1||signature>4096)throw std::runtime_error("invalid IoStore signature size");
        offset+=4+static_cast<uint64_t>(signature)*2+static_cast<uint64_t>(blocks)*20;
    }
    if(offset>size||length>size-offset)throw std::runtime_error("truncated IoStore directory index");
    PakSample sample;sample.path=path;sample.version=header[16];sample.indexOffset=offset;sample.serializedIndexOffset=offset;
    sample.ioStore=true;sample.chunkCount=count;std::memcpy(sample.guid.data(),header+64,16);
    sample.cipher.resize(length);f.seekg(static_cast<std::streamoff>(offset));
    if(!f.read(reinterpret_cast<char*>(sample.cipher.data()),length))throw std::runtime_error("cannot read IoStore directory index");
    return sample;
}

// Keep the smallest index for each explicit GUID to limit verification cost.
// A zero GUID is unspecified, so every such container must remain eligible.
// A verified result applies to its recorded GUID/container, not to every Pak.
inline std::vector<PakSample> discoverPaks(const fs::path& path, std::vector<std::string>& warnings) {
    const auto loadSample=[](const fs::path& file){return !_wcsicmp(file.extension().c_str(),L".utoc")?loadUtoc(file):loadPak(file);};
    if(fs::is_regular_file(path))return {loadSample(path)};
    if(!fs::is_directory(path))throw std::runtime_error("Pak directory does not exist: "+path.u8string());
    std::vector<PakSample> result;
    for(const auto& entry:fs::directory_iterator(path)) {
        if(!entry.is_regular_file() || (_wcsicmp(entry.path().extension().c_str(),L".pak")&&_wcsicmp(entry.path().extension().c_str(),L".utoc")))continue;
        try {
            auto sample=loadSample(entry.path());
            if(sample.guid==std::array<uint8_t,16>{}) {result.push_back(std::move(sample));continue;}
            auto it=std::find_if(result.begin(),result.end(),[&](const auto& x){return x.guid==sample.guid&&x.ioStore==sample.ioStore;});
            if(it==result.end())result.push_back(std::move(sample));
            else if(sample.cipher.size()<it->cipher.size())*it=std::move(sample);
        } catch(const std::exception& e){warnings.push_back(e.what());}
    }
    if(result.empty())throw std::runtime_error("no supported encrypted Pak/IoStore index found in "+path.u8string());
    std::sort(result.begin(),result.end(),[](const auto& a,const auto& b){return a.cipher.size()<b.cipher.size();});
    return result;
}

// IoStore directory indexes have no stored digest. Accept only a complete,
// connected directory/file graph with bounded names and valid chunk references.
// This is reported separately from cryptographic Pak index SHA-1 verification.
inline bool validIoStoreDirectory(const std::vector<uint8_t>& plain,uint32_t chunks) {
    constexpr uint32_t invalid=0xffffffff;
    std::size_t at=0;
    const auto word=[&](uint32_t& value){
        if(at>plain.size()||plain.size()-at<4)return false;
        value=load<uint32_t>(plain.data()+at);at+=4;return true;
    };
    const auto string=[&](bool name){
        uint32_t raw=0;if(!word(raw))return false;
        const auto length=static_cast<int32_t>(raw);
        const auto units=length<0?-static_cast<int64_t>(length):static_cast<int64_t>(length);
        const auto width=length<0?2u:1u;
        if(units>1048576||static_cast<uint64_t>(units)*width>plain.size()-at||(!units&&name))return false;
        for(int64_t i=0;i<units;++i) {
            const auto* p=plain.data()+at+static_cast<std::size_t>(i)*width;
            const auto c=width==1?static_cast<uint16_t>(*p):load<uint16_t>(p);
            if(i==units-1) {if(c)return false;}
            else if(c<32||c==127||(name&&(c=='/'||c=='\\')))return false;
        }
        at+=static_cast<std::size_t>(units)*width;return true;
    };
    if(!string(false))return false;
    uint32_t dirCount=0,fileCount=0,nameCount=0;
    if(!word(dirCount)||!dirCount||dirCount>(plain.size()-at)/16)return false;
    const auto dirs=at;at+=static_cast<std::size_t>(dirCount)*16;
    if(!word(fileCount)||!fileCount||fileCount>(plain.size()-at)/12)return false;
    const auto files=at;at+=static_cast<std::size_t>(fileCount)*12;
    if(!word(nameCount)||!nameCount||nameCount>(plain.size()-at)/5)return false;
    for(uint32_t i=0;i<nameCount;++i)if(!string(true))return false;
    if(plain.size()-at>15)return false;
    std::vector<uint8_t> visitedDirs(dirCount),visitedFiles(fileCount);
    std::vector<uint32_t> pending{0};uint32_t seenDirs=0,seenFiles=0;
    while(!pending.empty()) {
        const auto index=pending.back();pending.pop_back();
        if(index==invalid)continue;
        if(index>=dirCount||visitedDirs[index])return false;
        visitedDirs[index]=1;++seenDirs;
        const auto* dir=plain.data()+dirs+static_cast<std::size_t>(index)*16;
        if(index&&load<uint32_t>(dir)>=nameCount)return false;
        pending.push_back(load<uint32_t>(dir+4));pending.push_back(load<uint32_t>(dir+8));
        auto file=load<uint32_t>(dir+12);
        while(file!=invalid) {
            if(file>=fileCount||visitedFiles[file])return false;
            visitedFiles[file]=1;++seenFiles;
            const auto* entry=plain.data()+files+static_cast<std::size_t>(file)*12;
            if(load<uint32_t>(entry)>=nameCount||load<uint32_t>(entry+8)>=chunks)return false;
            file=load<uint32_t>(entry+4);
        }
    }
    return seenDirs==dirCount&&seenFiles==fileCount;
}

inline bool hashMatches(const std::vector<uint8_t>& plain,const std::array<uint8_t,20>& expected) {
    uint8_t digest[20]; calc_sha1(plain.data(),plain.size(),digest);
    if(!std::memcmp(digest,expected.data(),20))return true;
    // Pak writers can hash the original index and fill the final AES block
    // with zero, copied, or arbitrary bytes. The digest determines its length.
    for(std::size_t pad=1;pad<=15 && pad<plain.size();++pad) {
        calc_sha1(plain.data(),plain.size()-pad,digest);
        if(!std::memcmp(digest,expected.data(),20))return true;
    }
    return false;
}

inline bool verifyKey(const Aes256DecKey& key,const PakSample& sample,bool nte=false) {
    const auto& cipher=nte?sample.nteCipher:sample.cipher;
    if(cipher.empty())return false;
    std::vector<uint8_t> plain(cipher.size());
    for(std::size_t at=0;at<plain.size();at+=16)
        _mm_storeu_si128(reinterpret_cast<__m128i*>(plain.data()+at),
            aes256_decrypt_block(_mm_loadu_si128(reinterpret_cast<const __m128i*>(cipher.data()+at)),&key));
    return sample.ioStore?validIoStoreDirectory(plain,sample.chunkCount):hashMatches(plain,sample.digest);
}

inline bool plausibleMount(const Aes256DecKey& key,const PakSample& sample,const uint8_t* first,bool nte=false) {
    const auto& cipher=nte?sample.nteCipher:sample.cipher;
    const auto length=load<int32_t>(first);
    const int64_t units=length<0?-static_cast<int64_t>(length):length;
    const auto unit=length<0?2u:1u;
    const auto end=4+units*unit;
    if(units>1024*1024 || end>static_cast<int64_t>(cipher.size()))return false;
    if(!units)return true;
    const auto last=static_cast<std::size_t>(end-unit),block=last&~std::size_t(15);
    alignas(16) uint8_t tail[16];
    const uint8_t* terminator=nullptr;
    if(block) {
        _mm_storeu_si128(reinterpret_cast<__m128i*>(tail),aes256_decrypt_block(
            _mm_loadu_si128(reinterpret_cast<const __m128i*>(cipher.data()+block)),&key));
        terminator=tail+(last-block);
    } else terminator=first+last;
    if(unit==1?*terminator!=0:load<uint16_t>(terminator)!=0)return false;
    const auto available=std::min<int64_t>(units,12/unit);
    for(int64_t i=0;i<available;++i) {
        const auto* at=first+4+static_cast<std::size_t>(i)*unit;
        const auto c=unit==1?static_cast<uint16_t>(*at):load<uint16_t>(at);
        if(i+1==units) {if(c)return false;}
        else if(c<0x20 || c==0x7f)return false;
    }
    return true;
}

struct Options {
    fs::path pakPath,output;
    unsigned seconds=120,retryMs=2000;
    bool unaligned=true,snapshotModule=false;
};
struct MemoryRange {
    uint64_t base=0,size=0;
    DWORD type=0,protection=0;
    unsigned priority() const {
        const auto access=protection&0xff;
        const bool writable=access==PAGE_READWRITE || access==PAGE_WRITECOPY ||
                            access==PAGE_EXECUTE_READWRITE || access==PAGE_EXECUTE_WRITECOPY;
        const bool executable=access==PAGE_EXECUTE_READ || access==PAGE_EXECUTE_READWRITE || access==PAGE_EXECUTE_WRITECOPY;
        if(type==MEM_IMAGE && writable)return 0;
        if(type==MEM_IMAGE && !executable)return 1;
        if(type==MEM_PRIVATE && writable)return 2;
        if(type==MEM_PRIVATE)return 3;
        if(type==MEM_IMAGE)return 4;
        return 5;
    }
};
struct Result {
    std::string status="initializing",phase="loading",error,encoding,source,key;
    uint64_t address=0,bytes=0,candidates=0,readFailures=0,started=GetTickCount64(),elapsed=0;
    uint64_t unreadBytes=0,rangeAddress=0,rangeSize=0;
    unsigned sweeps=0,completedSweeps=0;
    std::size_t regions=0,regionIndex=0;
    DWORD rangeType=0,rangeProtection=0;
    DWORD pid=GetCurrentProcessId();
    std::string moduleSnapshot,evidenceError;
    std::size_t sampleIndex=0;
    int indexOffsetAdjustment=0;
    bool success=false;
    std::vector<std::string> warnings;
};

inline void atomicSave(const fs::path& path,const std::string& text) {
    const auto temp=fs::path(path.wstring()+L".tmp");
    {std::ofstream f(temp,std::ios::binary|std::ios::trunc);
     if(!f || !f.write(text.data(),static_cast<std::streamsize>(text.size())) || !f.flush())
         throw std::runtime_error("cannot write AES output: "+path.u8string());}
    DWORD error=0;
    // Native readers and antivirus can briefly deny delete-sharing. Keep the
    // previous complete state visible and retry the atomic rename.
    for(unsigned attempt=0;attempt<60;++attempt) {
        if(MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))return;
        error=GetLastError();
        if(error!=ERROR_SHARING_VIOLATION&&error!=ERROR_LOCK_VIOLATION&&error!=ERROR_ACCESS_DENIED)break;
        Sleep(25);
    }
    throw std::runtime_error("cannot commit AES output, Win32="+std::to_string(error));
}

struct ModuleSnapshot {
    fs::path metadata;
    uint64_t copiedBytes=0,unreadBytes=0;
    bool timedOut=false;
};

template<class Read> inline ModuleSnapshot saveModuleSnapshot(const fs::path& output,const ebon::Module& module,
        const std::vector<ebon::Region>& regions,const Read& read,unsigned budgetMs=15000) {
    if(!module.size || module.size>512ull*1024*1024 || module.base+module.size<module.base)
        throw std::runtime_error("AES module snapshot size is invalid or exceeds 512 MiB");
    const auto data=output/"module_memory.bin",temporary=output/"module_memory.bin.tmp";
    ModuleSnapshot result;result.metadata=output/"module_memory.json";
    if(fs::exists(data)||fs::exists(temporary)||fs::exists(result.metadata))
        throw std::runtime_error("AES module snapshot already exists");
    std::ofstream file(temporary,std::ios::binary|std::ios::trunc);
    if(!file)throw std::runtime_error("cannot create AES module snapshot");
    file.seekp(static_cast<std::streamoff>(module.size-1));file.put(0);
    if(!file)throw std::runtime_error("cannot size AES module snapshot");
    std::vector<uint8_t> buffer(chunkBytes);
    std::vector<std::pair<uint64_t,uint64_t>> captured;
    const auto started=GetTickCount64(),deadline=started+budgetMs;
    const auto commit=[&](uint64_t address,std::size_t count) {
        const auto rva=address-module.base;
        file.seekp(static_cast<std::streamoff>(rva));
        file.write(reinterpret_cast<const char*>(buffer.data()),static_cast<std::streamsize>(count));
        if(!file)throw std::runtime_error("cannot write AES module snapshot");
        result.copiedBytes+=count;
        if(!captured.empty()&&captured.back().first+captured.back().second==rva)captured.back().second+=count;
        else captured.emplace_back(rva,count);
    };
    for(const auto& region:regions) {
        if(region.address<module.base || region.address-module.base>=module.size)continue;
        const auto size=std::min<uint64_t>(region.size,module.size-(region.address-module.base));
        for(uint64_t offset=0;offset<size;) {
            if(GetTickCount64()>=deadline){result.timedOut=true;break;}
            const auto count=static_cast<std::size_t>(std::min<uint64_t>(chunkBytes,size-offset));
            if(read(region.address+offset,buffer.data(),count))commit(region.address+offset,count);
            else for(std::size_t page=0;page<count;page+=4096) {
                if(GetTickCount64()>=deadline){result.timedOut=true;break;}
                const auto bytes=std::min<std::size_t>(4096,count-page);
                if(read(region.address+offset+page,buffer.data(),bytes))commit(region.address+offset+page,bytes);
            }
            offset+=count;
            if(result.timedOut)break;
        }
        if(result.timedOut)break;
    }
    file.flush();if(!file)throw std::runtime_error("cannot flush AES module snapshot");file.close();
    if(!MoveFileExW(temporary.c_str(),data.c_str(),MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("cannot publish AES module snapshot, Win32="+std::to_string(GetLastError()));
    result.unreadBytes=module.size-result.copiedBytes;
    std::ostringstream metadata;
    metadata<<"{\"format\":\"EbonAesModuleSnapshot-v1\",\"layout\":\"virtual_address\",\"status\":"
        <<ebon::quote(result.timedOut?"timed_out":(result.unreadBytes?"partial":"complete"))
        <<",\"module\":"<<ebon::quote(ebon::utf8(module.name))<<",\"module_path\":"<<ebon::quote(ebon::utf8(module.path))
        <<",\"module_base\":"<<ebon::quote(ebon::hex(module.base))<<",\"module_size\":"<<module.size
        <<",\"data_file\":\"module_memory.bin\",\"copied_bytes\":"<<result.copiedBytes<<",\"unread_bytes\":"<<result.unreadBytes
        <<",\"elapsed_ms\":"<<GetTickCount64()-started<<",\"captured_ranges\":[";
    for(std::size_t i=0;i<captured.size();++i){if(i)metadata<<',';metadata<<'['<<captured[i].first<<','<<captured[i].second<<']';}
    metadata<<"]}\n";atomicSave(result.metadata,metadata.str());
    return result;
}

class Hunter {
    Options options_;
    std::vector<PakSample> samples_;
    uint64_t deadline_=0,lastSave_=0;
    bool evidencePending_=false;
    std::atomic_bool* cancel_=nullptr;
public:
    Result result;
    explicit Hunter(Options options,std::atomic_bool* cancel=nullptr):options_(std::move(options)),cancel_(cancel) {
        if(options_.seconds<1||options_.seconds>3600||options_.retryMs>60000)
            throw std::runtime_error("AES seconds must be 1..3600 and retry_ms 0..60000");
        if(fs::exists(options_.output)&&(!fs::is_directory(options_.output)||!fs::is_empty(options_.output)))
            throw std::runtime_error("AES output must be a new or empty directory");
        // Refuse writes under a supplied Pak directory or beside a supplied Pak.
        const auto root=fs::weakly_canonical(fs::is_directory(options_.pakPath)?options_.pakPath:options_.pakPath.parent_path()).wstring();
        const auto out=fs::weakly_canonical(options_.output).wstring();
        if(out.size()>=root.size() && CompareStringOrdinal(out.data(),static_cast<int>(root.size()),root.data(),static_cast<int>(root.size()),TRUE)==CSTR_EQUAL &&
           (out.size()==root.size() || out[root.size()]==L'\\' || out[root.size()]==L'/'))
            throw std::runtime_error("AES output must be outside the Pak directory");
        fs::create_directories(options_.output); save(true);
        try {
            if(!hasAesNi())throw std::runtime_error("AES-NI is unavailable on this CPU");
            samples_=discoverPaks(options_.pakPath,result.warnings);
            deadline_=GetTickCount64()+static_cast<uint64_t>(options_.seconds)*1000;
            result.status="scanning";save(true);
        }catch(const std::exception& e){fail(e.what());}
    }
    bool stopped() {
        if(result.success||result.status=="error")return true;
        if(cancel_&&cancel_->load()){result.status="cancelled";return true;}
        if(GetTickCount64()>=deadline_){result.status="timed_out";return true;}
        return false;
    }
    void fail(const std::string& error){result.error=error;result.status="error";result.success=false;save(true);}
    void save(bool force=false) {
        const auto now=GetTickCount64(); if(!force&&now-lastSave_<1000)return;lastSave_=now;
        result.elapsed=now-result.started;
        std::ostringstream s;
        const auto status=result.status=="timed_out"&&evidencePending_?"collecting_evidence":result.status;
        s<<"{\"format\":\"EbonAES-v1\",\"pid\":"<<result.pid<<",\"status\":"<<ebon::quote(status)
         <<",\"success\":"<<(result.success?"true":"false")<<",\"elapsed_ms\":"<<result.elapsed
         <<",\"phase\":"<<ebon::quote(result.phase)
         <<",\"sweeps\":"<<result.sweeps<<",\"bytes_scanned\":"<<result.bytes<<",\"candidates\":"<<result.candidates
         <<",\"completed_sweeps\":"<<result.completedSweeps<<",\"bytes_unread\":"<<result.unreadBytes
         <<",\"regions\":"<<result.regions<<",\"region_index\":"<<result.regionIndex
         <<",\"range_address\":"<<ebon::quote(ebon::hex(result.rangeAddress))<<",\"range_size\":"<<result.rangeSize
         <<",\"range_type\":"<<result.rangeType<<",\"range_protection\":"<<result.rangeProtection
         <<",\"module_snapshot\":"<<ebon::quote(result.moduleSnapshot)<<",\"evidence_error\":"<<ebon::quote(result.evidenceError)
         <<",\"read_failures\":"<<result.readFailures<<",\"source\":"<<ebon::quote(result.source)
         <<",\"address\":"<<ebon::quote(ebon::hex(result.address))<<",\"encoding\":"<<ebon::quote(result.encoding)
         <<",\"error\":"<<ebon::quote(result.error)<<",\"samples\":[";
        for(std::size_t i=0;i<samples_.size();++i){if(i)s<<',';const auto& p=samples_[i];
            const auto adjustment=result.success&&i==result.sampleIndex?result.indexOffsetAdjustment:p.offsetAdjustment;
            const auto effective=adjustment==-1?p.serializedIndexOffset-1:p.serializedIndexOffset;
            s<<"{\"pak\":"<<ebon::quote(p.path.u8string())<<",\"kind\":"<<ebon::quote(p.ioStore?"utoc":"pak")<<",\"guid\":"<<ebon::quote(hexBytes(p.guid.data(),16))
             <<",\"index_offset\":"<<effective<<",\"serialized_index_offset\":"<<p.serializedIndexOffset
             <<",\"index_offset_adjustment\":"<<adjustment<<",\"index_size\":"<<p.cipher.size()
             <<",\"index_sha1\":"<<(p.ioStore?"null":ebon::quote(hexBytes(p.digest.data(),20)))<<'}';}
        s<<"],\"warnings\":[";
        for(std::size_t i=0;i<result.warnings.size();++i){if(i)s<<',';s<<ebon::quote(result.warnings[i]);}
        s<<']';
        if(result.success)s<<",\"key\":"<<ebon::quote(result.key)<<",\"verified_sample\":"<<result.sampleIndex
            <<",\"verification_method\":"<<ebon::quote(samples_[result.sampleIndex].ioStore?"io_store_directory_structure":"pak_index_sha1");
        s<<"}\n"; atomicSave(options_.output/"aes_state.json",s.str());
    }
    bool candidate(const uint8_t* bytes,uint64_t address,const char* encoding) {
        ++result.candidates;
        Aes256DecKey key; aes256_set_decrypt_key(bytes,&key);
        for(std::size_t i=0;i<samples_.size();++i) {
            const auto& sample=samples_[i];
            for(const bool nte:{false,true}) {
                const auto& cipher=nte?sample.nteCipher:sample.cipher;
                if(cipher.empty())continue;
                alignas(16) uint8_t first[16];
                _mm_storeu_si128(reinterpret_cast<__m128i*>(first),aes256_decrypt_block(
                    _mm_loadu_si128(reinterpret_cast<const __m128i*>(cipher.data())),&key));
                if(!plausibleMount(key,sample,first,nte))continue;
                if(!verifyKey(key,sample,nte))continue;
                result.key="0x"+hexBytes(bytes,32);result.address=address;result.encoding=encoding;result.sampleIndex=i;
                result.indexOffsetAdjustment=sample.offsetAdjustment+(nte?-1:0);
                atomicSave(options_.output/"aes_key.txt",result.key+"\n");
                result.status="verified";result.success=true;save(true);return true;
            }
        }
        return false;
    }
    static int nibble(uint8_t c) {
        if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;
    }
    void scanImmediates(const uint8_t* bytes,std::size_t length,std::size_t starts,uint64_t base) {
        struct Store {int32_t offset=0;uint32_t value=0;unsigned frame=0;std::size_t size=0;};
        const auto decode=[&](std::size_t i,Store& store) {
            if(i+7>length||bytes[i]!=0xc7)return false;
            const auto mod=bytes[i+1];std::size_t disp=2;
            if(mod==0x45||mod==0x85)store.frame=5;
            else if((mod==0x44||mod==0x84)&&i+8<=length&&bytes[i+2]==0x24){store.frame=4;disp=3;}
            else return false;
            const auto wide=(mod&0xc0)==0x80;store.size=disp+(wide?4:1)+4;
            if(i+store.size>length)return false;
            store.offset=wide?load<int32_t>(bytes+i+disp):static_cast<int8_t>(bytes[i+disp]);
            store.value=load<uint32_t>(bytes+i+store.size-4);return true;
        };
        for(std::size_t i=0;i<starts&&i+7<=length;++i) {
            if(!(i&4095)){if(stopped())return;save();}
            Store first;if(!decode(i,first))continue;
            std::array<Store,32> stores{};std::size_t count=0;
            const auto end=std::min(length,i+256);
            for(std::size_t j=i;j<end;) {
                Store store;if(!decode(j,store)||j+store.size>end){++j;continue;}
                j+=store.size;
                if(store.frame!=first.frame)continue;
                auto slot=std::find_if(stores.begin(),stores.begin()+count,[&](const Store& x){return x.offset==store.offset;});
                if(slot!=stores.begin()+count)*slot=store;
                else if(count<stores.size())stores[count++]=store;
                if(count<8)continue;
                for(std::size_t start=0;start<count;++start) {
                    uint8_t key[32];bool complete=true;
                    for(int n=0;n<8;++n) {
                        const auto offset=static_cast<int64_t>(stores[start].offset)+n*4;
                        const auto found=std::find_if(stores.begin(),stores.begin()+count,[&](const Store& x){return x.offset==offset;});
                        if(found==stores.begin()+count){complete=false;break;}
                        std::memcpy(key+n*4,&found->value,4);
                    }
                    if(complete&&candidate(key,base+i,"immediate32"))return;
                }
            }
        }
    }
    void scan(const uint8_t* bytes,std::size_t length,std::size_t starts,uint64_t base,unsigned pass) {
        for(std::size_t i=0;i<starts && i+32<=length;++i) {
            if(!(i&4095)){if(stopped())return;save();}
            if(pass==0) {
                uint8_t decoded[32];
                for(const auto unit:{1u,2u}) {
                    if(i+64*unit>length || nibble(bytes[i])<0)continue;
                    bool valid=true;
                    for(std::size_t j=0;j<64;++j) {
                        const auto n=nibble(bytes[i+j*unit]);
                        if(n<0 || (unit==2&&bytes[i+j*unit+1])){valid=false;break;}
                        if(!(j&1))decoded[j/2]=static_cast<uint8_t>(n<<4);else decoded[j/2]|=static_cast<uint8_t>(n);
                    }
                    if(valid && candidate(decoded,base+i,unit==1?"hex":"utf16_hex"))return;
                }
            } else if((pass==1 && !((base+i)&7)) || (pass==2 && ((base+i)&7))) {
                // Empty memory is common; all-zero is a valid key and is checked once per chunk.
                if(i && !load<uint64_t>(bytes+i) && !load<uint64_t>(bytes+i+8) &&
                   !load<uint64_t>(bytes+i+16) && !load<uint64_t>(bytes+i+24))continue;
                if(candidate(bytes+i,base+i,"raw"))return;
            }
        }
    }
    template<class Read> void range(uint64_t base,uint64_t size,unsigned pass,const Read& read) {
        std::vector<uint8_t> buffer(chunkBytes+overlapBytes);
        const auto scanBuffer=[&](std::size_t length,std::size_t starts,uint64_t address) {
            if(pass==5){result.phase="instruction_keys";scanImmediates(buffer.data(),length,starts,address);return;}
            if(pass<3) {
                result.phase=pass==0?"text":(pass==1?"raw_aligned":"raw_unaligned");
                scan(buffer.data(),length,starts,address,pass);return;
            }
            // Mode 4 checks text and aligned keys together; mode 3 checks all
            // encodings. The slow unaligned pass can then run after coverage.
            const auto encodings=pass==4?2u:(options_.unaligned?3u:2u);
            for(unsigned encoding=0;encoding<encodings&&!stopped();++encoding) {
                result.phase=encoding==0?"text":(encoding==1?"raw_aligned":"raw_unaligned");
                scan(buffer.data(),length,starts,address,encoding);
            }
        };
        for(uint64_t at=0;at<size && !stopped();) {
            const auto body=static_cast<std::size_t>(std::min<uint64_t>(chunkBytes,size-at));
            const auto total=static_cast<std::size_t>(std::min<uint64_t>(body+overlapBytes,size-at));
            if(read(base+at,buffer.data(),total)) {
                scanBuffer(total,body,base+at);result.bytes+=body;
            }else{
                // A region may change after enumeration. Retry individual pages instead of losing the entire region.
                for(std::size_t p=0;p<body && !stopped();p+=4096) {
                    const auto count=std::min<std::size_t>(4096,body-p);
                    auto amount=static_cast<std::size_t>(std::min<uint64_t>(count+overlapBytes,size-at-p));
                    if(!read(base+at+p,buffer.data(),amount)) {
                        amount=count;
                        if(!read(base+at+p,buffer.data(),amount)){++result.readFailures;result.unreadBytes+=count;continue;}
                    }
                    scanBuffer(amount,count,base+at+p);result.bytes+=count;
                }
            }
            at+=body;save();
        }
    }
    void finish() {
        if(result.status=="scanning")result.status="exhausted";
        save(true);
    }
    template<class Read> void ranges(std::vector<MemoryRange> regions,const Read& read) {
        // Module data precedes heap allocations: a world can hold several GiB
        // of private memory and otherwise starve read-only constants entirely.
        std::sort(regions.begin(),regions.end(),[](const auto& a,const auto& b) {
            if(a.priority()!=b.priority())return a.priority()<b.priority();
            if(a.size!=b.size)return a.size<b.size;
            return a.base<b.base;
        });
        result.regions=regions.size();
        // Key getters can construct all eight words on the stack; scanning
        // instruction operands avoids waiting for a transient 32-byte buffer.
        for(std::size_t index=0;index<regions.size();++index) {
            const auto& region=regions[index];
            const auto access=region.protection&0xff;
            if(region.type!=MEM_IMAGE||(access!=PAGE_EXECUTE_READ&&access!=PAGE_EXECUTE_READWRITE&&access!=PAGE_EXECUTE_WRITECOPY))continue;
            if(stopped())break;
            result.regionIndex=index;
            result.rangeAddress=region.base;result.rangeSize=region.size;
            result.rangeType=region.type;result.rangeProtection=region.protection;
            range(region.base,region.size,5,read);
        }
        // Cover every range with text/aligned candidates before spending the
        // remaining time on the seven additional unaligned candidates per 8 bytes.
        for(const auto pass:{4u,2u}) {
            if(pass==2 && !options_.unaligned)break;
            result.regionIndex=0;
            for(const auto& region:regions) {
                if(stopped())break;
                result.rangeAddress=region.base;result.rangeSize=region.size;
                result.rangeType=region.type;result.rangeProtection=region.protection;
                if(pass==2)result.phase="raw_unaligned";
                range(region.base,region.size,pass,read);
                ++result.regionIndex;
            }
            if(stopped())break;
        }
    }
    void memory(HANDLE process=nullptr) {
        result.pid=process?GetProcessId(process):GetCurrentProcessId();
        evidencePending_=!process&&options_.snapshotModule;
        result.phase="enumerating";save(true);
        const auto query=[&](uint64_t at,MEMORY_BASIC_INFORMATION& mbi){
            return process?VirtualQueryEx(process,reinterpret_cast<void*>(at),&mbi,sizeof(mbi)):VirtualQuery(reinterpret_cast<void*>(at),&mbi,sizeof(mbi));};
        const ebon::InProcessReader localReader;
        const auto read=[&](uint64_t at,void* out,std::size_t n) {
            // Recheck current page protections after enumeration; transient heap
            // ranges can be released or become unreadable during a long sweep.
            if(!process)return localReader.copy(at,out,n);
            SIZE_T got=0;return ReadProcessMemory(process,reinterpret_cast<void*>(at),out,n,&got)&&got==n;
        };
        while(!stopped()) {
            ++result.sweeps;
            result.phase="enumerating";save(true);
            std::vector<MemoryRange> regions;
            for(uint64_t at=0;!stopped();) {
                MEMORY_BASIC_INFORMATION mbi{};if(!query(at,mbi))break;
                const auto base=reinterpret_cast<uint64_t>(mbi.BaseAddress),next=base+mbi.RegionSize;
                if(next<=at)break;
                if(mbi.State==MEM_COMMIT && ebon::readableProtection(mbi.Protect)) {
                    uint64_t size=mbi.RegionSize;
                    MEMORY_BASIC_INFORMATION neighbor{};
                    if(query(next,neighbor)&&neighbor.State==MEM_COMMIT&&ebon::readableProtection(neighbor.Protect))
                        size+=std::min<std::size_t>(overlapBytes,neighbor.RegionSize);
                    regions.push_back({base,size,mbi.Type,mbi.Protect});
                }
                at=next;
            }
            result.source=process?"process_memory":"in_process_memory";
            ranges(std::move(regions),read);
            if(!stopped())++result.completedSweeps;
            const auto until=GetTickCount64()+options_.retryMs;
            while(!stopped()&&GetTickCount64()<until)Sleep(25);
        }
        if(!process && options_.snapshotModule && result.status=="timed_out") {
            result.status="collecting_evidence";result.phase="module_snapshot";save(true);
            try {
                const auto image=GetModuleHandleW(nullptr);
                MODULEINFO info{};
                if(!image||!GetModuleInformation(GetCurrentProcess(),image,&info,sizeof(info)))
                    throw std::runtime_error("cannot locate main module for AES snapshot");
                wchar_t path[32768]{};
                if(!GetModuleFileNameW(image,path,32768))throw std::runtime_error("cannot identify AES snapshot module");
                ebon::Module module;module.base=reinterpret_cast<uint64_t>(info.lpBaseOfDll);module.size=info.SizeOfImage;
                module.path=path;module.name=fs::path(path).filename().wstring();
                const auto snapshot=saveModuleSnapshot(options_.output,module,localReader.regions(module),read);
                result.moduleSnapshot=snapshot.metadata.u8string();
            }catch(const std::exception& error){result.evidenceError=error.what();}
            result.status="timed_out";
        }
        evidencePending_=false;
        finish();
    }
    void files(const std::vector<fs::path>& paths) {
        ++result.sweeps;
        for(const auto pass:{5u,0u,1u,2u}) {
            if(stopped()||(pass==2&&!options_.unaligned))break;
            for(const auto& path:paths) {
                if(stopped())break;
                std::ifstream file(path,std::ios::binary);if(!file)throw std::runtime_error("cannot read scan file: "+path.u8string());
                file.seekg(0,std::ios::end);const auto end=file.tellg();if(end<0)throw std::runtime_error("cannot size scan file");
                result.source=path.u8string();
                range(0,static_cast<uint64_t>(end),pass,[&](uint64_t at,void* out,std::size_t n){
                    file.clear();file.seekg(static_cast<std::streamoff>(at));return static_cast<bool>(file.read(static_cast<char*>(out),static_cast<std::streamsize>(n)));});
            }
        }
        finish();
    }
};

inline Options fromConfig(const ebon::Config& config,const fs::path& output) {
    Options options;options.output=output;
    const auto seconds=config.number("aes_timeout_seconds",120),retry=config.number("aes_retry_ms",2000);
    if(seconds<1||seconds>3600||retry<0||retry>60000)throw std::runtime_error("invalid AES timeout/retry profile option");
    options.seconds=static_cast<unsigned>(seconds);options.retryMs=static_cast<unsigned>(retry);
    options.unaligned=config.number("aes_unaligned",1)!=0;
    options.snapshotModule=config.number("aes_snapshot_module",1)!=0;
    const auto pak=config.text("aes_pak");
    options.pakPath=pak.empty()?fs::path(ebon::wide(config.text("client_root")))/"Client"/"WindowsNoEditor"/"HT"/"Content"/"Paks":fs::path(ebon::wide(pak));
    return options;
}

// DLL reference is held for the entire asynchronous scan; reflection can complete independently.
struct BackgroundJob { std::unique_ptr<Hunter> hunter; HMODULE module=nullptr; };
inline DWORD WINAPI backgroundWorker(void* ptr) {
    auto* job=static_cast<BackgroundJob*>(ptr);const auto module=job->module;
    try{try{job->hunter->memory();}catch(const std::exception& e){job->hunter->fail(e.what());}}
    catch(const std::exception& e){OutputDebugStringA((std::string("[ebon_aes] ")+e.what()+"\n").c_str());}
    delete job;FreeLibraryAndExitThread(module,0);
}
inline void startBackground(const Options& options) {
    auto job=std::make_unique<BackgroundJob>();
    // Publish a state before dispatch. Constructor/path failures are returned to
    // the injection worker, which can record them inside its validated capture.
    job->hunter=std::make_unique<Hunter>(options);
    HMODULE module=nullptr;
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,reinterpret_cast<LPCWSTR>(&backgroundWorker),&module))
        throw std::runtime_error("cannot retain DLL for AES worker");
    job->module=module;
    const auto thread=CreateThread(nullptr,0,backgroundWorker,job.get(),0,nullptr);
    if(!thread){FreeLibrary(module);throw std::runtime_error("cannot start AES worker");}
    job.release();
    CloseHandle(thread);
}
} // namespace ebon_hunter
