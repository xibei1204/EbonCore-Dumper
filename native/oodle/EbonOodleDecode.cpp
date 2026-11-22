// C++17 | EbonOodleDecode.cpp | Windows x64/MSVC | bounded Oodle stream decoder.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

int Kraken_Decompress(const unsigned char* src,size_t src_len,unsigned char* dst,size_t dst_len);
constexpr size_t max_bytes=512ull*1024*1024;
constexpr size_t guard_bytes=64;

static int decode(const unsigned char* src,size_t src_len,unsigned char* dst,size_t dst_len) {
    __try{return Kraken_Decompress(src,src_len,dst,dst_len);}
    __except(EXCEPTION_EXECUTE_HANDLER){return -1;}
}

int main(int argc,char** argv) {
    try {
        if(argc==2&&!std::strcmp(argv[1],"--help")) {
            std::cout<<"EbonOodleDecode --size BYTES < compressed-stream > decoded-stream\n";return 0;
        }
        if(argc!=3||std::strcmp(argv[1],"--size"))throw std::runtime_error("expected --size BYTES");
        char* end=nullptr;errno=0;
        const auto requested=std::strtoull(argv[2],&end,10);
        if(argv[2][0]<'0'||argv[2][0]>'9'||errno||*end||requested>max_bytes)
            throw std::runtime_error("output size must be 0..536870912 bytes");
        _setmode(_fileno(stdin),_O_BINARY);_setmode(_fileno(stdout),_O_BINARY);
        std::vector<unsigned char> source;char chunk[65536];
        while(std::cin.read(chunk,sizeof(chunk))||std::cin.gcount()) {
            const auto count=static_cast<size_t>(std::cin.gcount());
            if(source.size()>max_bytes-count)throw std::runtime_error("compressed input exceeds 512 MiB");
            source.insert(source.end(),chunk,chunk+count);
        }
        if(std::cin.bad())throw std::runtime_error("cannot read compressed input");
        const auto source_size=source.size();source.resize(source_size+guard_bytes);
        std::vector<unsigned char> output(static_cast<size_t>(requested)+guard_bytes);
        const auto written=decode(source.data(),source_size,output.data(),static_cast<size_t>(requested));
        if(written<0||static_cast<unsigned long long>(written)!=requested)
            throw std::runtime_error("invalid Oodle stream or decoded size mismatch");
        std::cout.write(reinterpret_cast<const char*>(output.data()),static_cast<std::streamsize>(requested));
        std::cout.flush();return std::cout.good()?0:1;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
