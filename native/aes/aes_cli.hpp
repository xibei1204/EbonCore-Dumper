#pragma once
#include "dumper/ebon_aes_hunter.hpp"

inline int aesMain(int argc,wchar_t** argv,bool memoryMode) {
    HANDLE process=nullptr;
    try {
        ebon_hunter::Options options;
        std::vector<std::filesystem::path> files;
        DWORD pid=0;
        for(int i=1;i<argc;++i) {
            const std::wstring arg=argv[i];
            const auto value=[&](){if(i+1>=argc)throw std::runtime_error("missing option value");return std::wstring(argv[++i]);};
            if(arg==L"--help"||arg==L"-h") {
                std::cout<<(memoryMode?"EbonAesScan --pid PID":"EbonAesFinder --file FILE [--file FILE ...]")
                         <<" --pak <Pak/UTOC file or directory> --out <new directory> [--seconds 120] [--retry-ms 2000] [--aligned-only]\n"
                         <<"Read-only AES-256 search. Verifies Pak index SHA-1 or the complete IoStore directory graph.\n";
                return 0;
            }
            if(arg==L"--pak")options.pakPath=value();
            else if(arg==L"--out")options.output=value();
            else if(arg==L"--file"&&!memoryMode)files.push_back(value());
            else if(arg==L"--pid"&&memoryMode){const auto text=value();std::size_t used=0;const auto n=std::stoll(text,&used);if(used!=text.size()||n<1||n>MAXDWORD)throw std::runtime_error("invalid PID");pid=static_cast<DWORD>(n);}
            else if(arg==L"--seconds"||arg==L"--retry-ms") {
                const auto text=value();std::size_t used=0;const auto n=std::stoll(text,&used);
                if(used!=text.size()||n<0||n>60000)throw std::runtime_error("invalid numeric option");
                if(arg==L"--seconds")options.seconds=static_cast<unsigned>(n);else options.retryMs=static_cast<unsigned>(n);
            }
            else if(arg==L"--aligned-only")options.unaligned=false;
            else throw std::runtime_error("unknown option: "+ebon::utf8(arg));
        }
        if(options.pakPath.empty()||options.output.empty() || (memoryMode?!pid:files.empty()))
            throw std::runtime_error("required options are missing; use --help");
        options.pakPath=std::filesystem::absolute(options.pakPath);
        options.output=std::filesystem::absolute(options.output);
        // Do not overwrite scan inputs, including through a parent output directory.
        for(const auto& file:files) {
            if(!std::filesystem::is_regular_file(file))throw std::runtime_error("scan file does not exist: "+file.u8string());
            const auto input=std::filesystem::weakly_canonical(file).wstring();
            const auto output=std::filesystem::weakly_canonical(options.output).wstring();
            if(input==output)throw std::runtime_error("output collides with a scan input");
        }
        ebon_hunter::Hunter hunter(options);
        if(memoryMode)hunter.result.pid=pid;
        try {
            if(hunter.result.status=="error") { /* Initialization already produced a terminal state. */ }
            else if(memoryMode) {
                process=OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ,FALSE,pid);
                if(!process)throw std::runtime_error("OpenProcess read-only failed, Win32="+std::to_string(GetLastError()));
                hunter.memory(process);
            }else hunter.files(files);
        } catch(const std::exception& e){hunter.fail(e.what());}
        if(process){CloseHandle(process);process=nullptr;}
        std::cout<<"AES status="<<hunter.result.status<<" candidates="<<hunter.result.candidates
                 <<" elapsed_ms="<<hunter.result.elapsed<<" state="<<(options.output/"aes_state.json").u8string()<<"\n";
        if(!hunter.result.error.empty())std::cerr<<hunter.result.error<<"\n";
        // The key is stored in aes_key.txt, not printed into shared console logs.
        return hunter.result.success?0:(hunter.result.status=="error"?1:2);
    }catch(const std::exception& e){if(process)CloseHandle(process);std::cerr<<e.what()<<"\n";return 1;}
}
