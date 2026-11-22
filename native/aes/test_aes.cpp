#include "dumper/ebon_aes_hunter.hpp"
using namespace ebon_hunter;

int wmain(int argc,wchar_t** argv) {
    if(argc==3 && std::wstring(argv[1])==L"atomic") {
        const fs::path path=argv[2];atomicSave(path,"old");
        const auto reader=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
        if(reader==INVALID_HANDLE_VALUE)return 3;
        std::thread release([=](){Sleep(200);CloseHandle(reader);});
        bool ok=true;
        try{atomicSave(path,"new");}catch(...){ok=false;}
        release.join();
        std::ifstream f(path);std::string text;f>>text;
        return ok&&text=="new"?0:4;
    }
    if(!hasAesNi()){std::cerr<<"AES-NI unavailable\n";return 1;}
    // NIST Test Vector for AES-256 ECB
    // Key: 000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f
    // Plaintext: 00112233445566778899aabbccddeeff
    // Ciphertext: 8ea2b7ca516745bfeafc49904b496089
    const uint8_t key[32] = {
        0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,
        0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f
    };
    if(argc==4) {
        Options options;options.pakPath=argv[2];options.output=argv[3];options.seconds=10;
        const auto mode=std::wstring(argv[1]);
        if(mode==L"chunk")options.seconds=1;
        Hunter hunter(options);
        if(mode==L"priority" || mode==L"image_priority" || mode==L"chunk") {
            const bool priority=mode!=L"chunk";
            const uint64_t base=priority?uint64_t(1)<<40:0;
            std::vector<uint8_t> memory(priority?4096:chunkBytes*2);
            const auto keyOffset=priority?8u:7u;
            std::memcpy(memory.data()+keyOffset,key,32);
            unsigned reads=0;
            const auto read=[&](uint64_t at,void* out,std::size_t n) {
                ++reads;
                if(at<base || at-base>memory.size() || n>memory.size()-static_cast<std::size_t>(at-base))return false;
                if(mode==L"chunk" && at>=chunkBytes)Sleep(1100);
                std::memcpy(out,memory.data()+static_cast<std::size_t>(at-base),n);return true;
            };
            if(priority)hunter.ranges({{0,256u*1024u*1024u,static_cast<DWORD>(mode==L"image_priority"?MEM_PRIVATE:MEM_MAPPED),PAGE_READWRITE},
                                       {base,memory.size(),static_cast<DWORD>(mode==L"image_priority"?MEM_IMAGE:MEM_PRIVATE),
                                        static_cast<DWORD>(mode==L"image_priority"?PAGE_READONLY:PAGE_READWRITE)}},read);
            else hunter.range(base,memory.size(),3,read);
            hunter.finish();
            return hunter.result.success && hunter.result.address==base+keyOffset && reads==1 ? 0:2;
        }
        if(mode==L"snapshot") {
            std::vector<uint8_t> memory(12288,0x41);
            ebon::Module module;module.base=0x10000;module.size=memory.size();module.name=L"fixture.exe";module.path=L"fixture.exe";
            const auto read=[&](uint64_t at,void* out,std::size_t n) {
                const auto offset=at-module.base;
                if(at<module.base || offset>memory.size() || n>memory.size()-static_cast<std::size_t>(offset))return false;
                if(offset<8192 && offset+n>4096)return false;
                std::memcpy(out,memory.data()+static_cast<std::size_t>(offset),n);return true;
            };
            const auto snapshot=saveModuleSnapshot(options.output,module,{{module.base,module.size,false,false}},read);
            hunter.finish();
            return snapshot.copiedBytes==8192 && snapshot.unreadBytes==4096 && !snapshot.timedOut ? 0:2;
        }
        if(mode==L"coverage") {
            std::vector<uint8_t> memory(256u*1024u,0xff);
            const auto boundary=memory.size()/2;
            std::memcpy(memory.data()+boundary+8,key,32);
            const auto read=[&](uint64_t at,void* out,std::size_t n) {
                if(at>memory.size() || n>memory.size()-static_cast<std::size_t>(at))return false;
                std::memcpy(out,memory.data()+at,n);return true;
            };
            hunter.ranges({{0,boundary,MEM_PRIVATE,PAGE_READWRITE},
                           {boundary,boundary,MEM_PRIVATE,PAGE_READWRITE}},read);
            hunter.finish();
            return hunter.result.success && hunter.result.address==boundary+8 &&
                   hunter.result.candidates<boundary/4 ? 0:2;
        }
        if(mode==L"protection") {
            auto* memory=static_cast<uint8_t*>(VirtualAlloc(nullptr,12288,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
            if(!memory)return 3;
            std::memcpy(memory+8199,key,32);
            DWORD previous=0;
            if(!VirtualProtect(memory+4096,4096,PAGE_READWRITE|PAGE_GUARD,&previous)) {
                VirtualFree(memory,0,MEM_RELEASE);return 3;
            }
            const ebon::InProcessReader reader;
            const auto base=reinterpret_cast<uint64_t>(memory);
            hunter.range(base,12288,3,[&](uint64_t at,void* out,std::size_t n){return reader.copy(at,out,n);});
            hunter.finish();
            MEMORY_BASIC_INFORMATION guarded{};
            const bool guardPreserved=VirtualQuery(memory+4096,&guarded,sizeof(guarded)) && (guarded.Protect&PAGE_GUARD);
            const bool found=hunter.result.success && hunter.result.address==base+8199 && hunter.result.unreadBytes==4096;
            VirtualFree(memory,0,MEM_RELEASE);
            return found && guardPreserved ? 0:2;
        }
        const auto offset=mode==L"large"?128u*1024u*1024u+53u:8192u+7u;
        std::vector<uint8_t> memory(static_cast<std::size_t>(offset)+64);
        std::memcpy(memory.data()+offset,key,32);
        const auto read=[&](uint64_t at,void* out,std::size_t n) {
            if(at>memory.size()||n>memory.size()-static_cast<std::size_t>(at))return false;
            if(mode==L"gap" && at<8192 && at+n>4096)return false;
            std::memcpy(out,memory.data()+at,n);return true;
        };
        // Test the real range reader with a faulting page or a >128 MiB region.
        hunter.range(0,memory.size(),2,read);hunter.finish();
        return hunter.result.success&&hunter.result.address==offset ? 0:2;
    }
    const uint8_t cipher[16] = {
        0x8e,0xa2,0xb7,0xca,0x51,0x67,0x45,0xbf,0xea,0xfc,0x49,0x90,0x4b,0x49,0x60,0x89
    };
    const uint8_t expected_plain[16] = {
        0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff
    };

    Aes256DecKey dkey;
    aes256_set_decrypt_key(key, &dkey);

    __m128i c = _mm_loadu_si128((const __m128i*)cipher);
    __m128i p = aes256_decrypt_block(c, &dkey);

    uint8_t result[16];
    _mm_storeu_si128((__m128i*)result, p);

    bool match = (memcmp(result, expected_plain, 16) == 0);
    const uint8_t shaExpected[20]={0xa9,0x99,0x3e,0x36,0x47,0x06,0x81,0x6a,0xba,0x3e,0x25,0x71,0x78,0x50,0xc2,0x6c,0x9c,0xd0,0xd8,0x9d};
    uint8_t sha[20];calc_sha1(reinterpret_cast<const uint8_t*>("abc"),3,sha);
    match=match && !memcmp(sha,shaExpected,20);
    PakSample sample;sample.cipher.assign(cipher,cipher+16);
    calc_sha1(expected_plain,16,sample.digest.data());
    match=match && verifyKey(dkey,sample);
    sample.digest[0]^=1;
    match=match && !verifyKey(dkey,sample);
    std::cout << "AES-NI test: " << (match ? "PASS" : "FAIL") << "\n";
    return match ? 0 : 1;
}
