#pragma once
#include "ebon_aes_hunter.hpp"

namespace ebon_hunter {
namespace fs = std::filesystem;
constexpr std::size_t maxIndexBytes = 256 * 1024 * 1024;
constexpr std::size_t chunkBytes = 1024 * 1024;
constexpr std::size_t overlapBytes = 127; // Longest candidate is 64 UTF-16 hex digits.

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
    std::array<uint8_t,20> digest{};
    std::array<uint8_t,16> guid{};
    std::vector<uint8_t> cipher;
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
    if(length<16 || length>maxIndexBytes || length%16 || offset>size-221 || length>size-221-offset)
        throw std::runtime_error("invalid encrypted Pak index range: "+path.u8string());
    PakSample sample; sample.path=path; sample.version=version; sample.indexOffset=offset;
    std::memcpy(sample.digest.data(),footer+41,20); std::memcpy(sample.guid.data(),footer,16);
    sample.cipher.resize(static_cast<std::size_t>(length));
    f.seekg(static_cast<std::streamoff>(offset));
    if(!f.read(reinterpret_cast<char*>(sample.cipher.data()),static_cast<std::streamsize>(length)))
        throw std::runtime_error("truncated encrypted Pak index");
    return sample;
}

// Keep the smallest index for each GUID to limit candidate verification cost.
// A verified result applies to its recorded GUID/container, not to every Pak.
inline std::vector<PakSample> discoverPaks(const fs::path& path, std::vector<std::string>& warnings) {
    if(fs::is_regular_file(path))return {loadPak(path)};
    if(!fs::is_directory(path))throw std::runtime_error("Pak directory does not exist: "+path.u8string());
    std::vector<PakSample> result;
    for(const auto& entry:fs::directory_iterator(path)) {
        if(!entry.is_regular_file() || _wcsicmp(entry.path().extension().c_str(),L".pak"))continue;
        try {
            auto sample=loadPak(entry.path());
            auto it=std::find_if(result.begin(),result.end(),[&](const auto& x){return x.guid==sample.guid;});
            if(it==result.end())result.push_back(std::move(sample));
            else if(sample.cipher.size()<it->cipher.size())*it=std::move(sample);
        } catch(const std::exception& e){warnings.push_back(e.what());}
    }
    if(result.empty())throw std::runtime_error("no supported encrypted Pak index found in "+path.u8string());
    std::sort(result.begin(),result.end(),[](const auto& a,const auto& b){return a.cipher.size()<b.cipher.size();});
    return result;
}

inline bool hashMatches(const std::vector<uint8_t>& plain,const std::array<uint8_t,20>& expected) {
    uint8_t digest[20]; calc_sha1(plain.data(),plain.size(),digest);
    if(!std::memcmp(digest,expected.data(),20))return true;
    for(std::size_t pad=1;pad<=15 && pad<plain.size();++pad) {
        if(plain[plain.size()-pad])break;
        calc_sha1(plain.data(),plain.size()-pad,digest);
        if(!std::memcmp(digest,expected.data(),20))return true;
    }
    return false;
}

inline bool verifyKey(const Aes256DecKey& key,const PakSample& sample) {
    std::vector<uint8_t> plain(sample.cipher.size());
    for(std::size_t at=0;at<plain.size();at+=16)
        _mm_storeu_si128(reinterpret_cast<__m128i*>(plain.data()+at),
            aes256_decrypt_block(_mm_loadu_si128(reinterpret_cast<const __m128i*>(sample.cipher.data()+at)),&key));
    return hashMatches(plain,sample.digest);
}

struct Options {
    fs::path pakPath,output;
    unsigned seconds=120,retryMs=2000;
    bool unaligned=true;
};
struct Result {
    std::string status="initializing",phase="loading",error,encoding,source,key;
    uint64_t address=0,bytes=0,candidates=0,readFailures=0,started=GetTickCount64(),elapsed=0;
    unsigned sweeps=0;
    DWORD pid=GetCurrentProcessId();
    std::size_t sampleIndex=0;
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

class Hunter {
    Options options_;
    std::vector<PakSample> samples_;
    uint64_t deadline_=0,lastSave_=0;
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
        s<<"{\"format\":\"EbonAES-v1\",\"pid\":"<<result.pid<<",\"status\":"<<ebon::quote(result.status)
         <<",\"success\":"<<(result.success?"true":"false")<<",\"elapsed_ms\":"<<result.elapsed
         <<",\"phase\":"<<ebon::quote(result.phase)
         <<",\"sweeps\":"<<result.sweeps<<",\"bytes_scanned\":"<<result.bytes<<",\"candidates\":"<<result.candidates
         <<",\"read_failures\":"<<result.readFailures<<",\"source\":"<<ebon::quote(result.source)
         <<",\"address\":"<<ebon::quote(ebon::hex(result.address))<<",\"encoding\":"<<ebon::quote(result.encoding)
         <<",\"error\":"<<ebon::quote(result.error)<<",\"samples\":[";
        for(std::size_t i=0;i<samples_.size();++i){if(i)s<<',';const auto& p=samples_[i];
            s<<"{\"pak\":"<<ebon::quote(p.path.u8string())<<",\"guid\":"<<ebon::quote(hexBytes(p.guid.data(),16))
             <<",\"index_offset\":"<<p.indexOffset<<",\"index_size\":"<<p.cipher.size()
             <<",\"index_sha1\":"<<ebon::quote(hexBytes(p.digest.data(),20))<<'}';}
        s<<"],\"warnings\":[";
        for(std::size_t i=0;i<result.warnings.size();++i){if(i)s<<',';s<<ebon::quote(result.warnings[i]);}
        s<<']';
        if(result.success)s<<",\"key\":"<<ebon::quote(result.key)<<",\"verified_sample\":"<<result.sampleIndex;
        s<<"}\n"; atomicSave(options_.output/"aes_state.json",s.str());
    }
    bool candidate(const uint8_t* bytes,uint64_t address,const char* encoding) {
        ++result.candidates;
        Aes256DecKey key; aes256_set_decrypt_key(bytes,&key);
        for(std::size_t i=0;i<samples_.size();++i) {
            const auto& sample=samples_[i];
            alignas(16) uint8_t first[16];
            _mm_storeu_si128(reinterpret_cast<__m128i*>(first),aes256_decrypt_block(
                _mm_loadu_si128(reinterpret_cast<const __m128i*>(sample.cipher.data())),&key));
            const auto len=load<int32_t>(first);
            // This only rejects impossible FString lengths. The SHA-1 is the acceptance test.
            const int64_t units=len<0?-static_cast<int64_t>(len):len;
            if(units>1024*1024 || 4+units*(len<0?2:1)>static_cast<int64_t>(sample.cipher.size()))continue;
            if(!verifyKey(key,sample))continue;
            result.key="0x"+hexBytes(bytes,32);result.address=address;result.encoding=encoding;result.sampleIndex=i;
            atomicSave(options_.output/"aes_key.txt",result.key+"\n");
            result.status="verified";result.success=true;save(true);return true;
        }
        return false;
    }
    static int nibble(uint8_t c) {
        if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;
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
        for(uint64_t at=0;at<size && !stopped();) {
            const auto body=static_cast<std::size_t>(std::min<uint64_t>(chunkBytes,size-at));
            const auto total=static_cast<std::size_t>(std::min<uint64_t>(body+overlapBytes,size-at));
            if(read(base+at,buffer.data(),total)) {
                scan(buffer.data(),total,body,base+at,pass);result.bytes+=body;
            }else{
                // A region may change after enumeration. Retry individual pages instead of losing the entire region.
                for(std::size_t p=0;p<body && !stopped();p+=4096) {
                    const auto count=std::min<std::size_t>(4096,body-p);
                    auto amount=static_cast<std::size_t>(std::min<uint64_t>(count+overlapBytes,size-at-p));
                    if(!read(base+at+p,buffer.data(),amount)) {
                        amount=count;
                        if(!read(base+at+p,buffer.data(),amount)){++result.readFailures;continue;}
                    }
                    scan(buffer.data(),amount,count,base+at+p,pass);result.bytes+=count;
                }
            }
            at+=body;save();
        }
    }
    void finish() {
        if(result.status=="scanning")result.status="exhausted";
        save(true);
    }
    void memory(HANDLE process=nullptr) {
        result.pid=process?GetProcessId(process):GetCurrentProcessId();
        result.phase="enumerating";save(true);
        const auto query=[&](uint64_t at,MEMORY_BASIC_INFORMATION& mbi){
            return process?VirtualQueryEx(process,reinterpret_cast<void*>(at),&mbi,sizeof(mbi)):VirtualQuery(reinterpret_cast<void*>(at),&mbi,sizeof(mbi));};
        const auto read=[&](uint64_t at,void* out,std::size_t n) {
            if(!process)return safe_memcpy(out,reinterpret_cast<const void*>(at),n);
            SIZE_T got=0;return ReadProcessMemory(process,reinterpret_cast<void*>(at),out,n,&got)&&got==n;
        };
        while(!stopped()) {
            ++result.sweeps;
            // Text pass, aligned raw pass, then only unaligned raw candidates.
            for(unsigned pass=0;pass<(options_.unaligned?3u:2u)&&!stopped();++pass) {
                result.phase=pass==0?"text":(pass==1?"raw_aligned":"raw_unaligned");save(true);
                for(uint64_t at=0;!stopped();) {
                    MEMORY_BASIC_INFORMATION mbi{};if(!query(at,mbi))break;
                    const auto base=reinterpret_cast<uint64_t>(mbi.BaseAddress),next=base+mbi.RegionSize;
                    if(next<=at)break;
                    if(mbi.State==MEM_COMMIT && ebon::readableProtection(mbi.Protect)) {
                        result.source=process?"process_memory":"in_process_memory";
                        // Include readable neighboring regions so candidates spanning region boundaries are tested.
                        uint64_t size=mbi.RegionSize;
                        MEMORY_BASIC_INFORMATION neighbor{};
                        if(query(next,neighbor)&&neighbor.State==MEM_COMMIT&&ebon::readableProtection(neighbor.Protect))
                            size+=std::min<std::size_t>(overlapBytes,neighbor.RegionSize);
                        range(base,size,pass,read);
                    }
                    at=next;
                }
            }
            const auto until=GetTickCount64()+options_.retryMs;
            while(!stopped()&&GetTickCount64()<until)Sleep(25);
        }
        finish();
    }
    void files(const std::vector<fs::path>& paths) {
        ++result.sweeps;
        for(unsigned pass=0;pass<(options_.unaligned?3u:2u)&&!stopped();++pass) {
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
