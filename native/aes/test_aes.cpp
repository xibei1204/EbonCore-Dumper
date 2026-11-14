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
        Hunter hunter(options);
        const auto mode=std::wstring(argv[1]);
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
