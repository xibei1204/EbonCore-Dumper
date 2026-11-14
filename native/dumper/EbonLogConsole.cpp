// Separate console owner: the game never allocates or attaches a console.
#include "../common/ebon_console.hpp"

int wmain(int argc,wchar_t** argv) {
    std::filesystem::path log;
    DWORD pid=0;
    for(int i=1;i+1<argc;i+=2){
        const std::wstring option=argv[i];
        if(option==L"--log")log=argv[i+1];
        else if(option==L"--pid")pid=wcstoul(argv[i+1],nullptr,10);
        else return 2;
    }
    if(log.empty()||!pid)return 2;
    const auto title=L"EbonCore Dumper - game PID "+std::to_wstring(pid);
    if(!ebon::console::initialize(title.c_str()))return 1;
    ebon::console::emitRaw("EbonCore Dumper | independent log console\r\nClosing this window leaves the game running.\r\n\r\n");
    HANDLE target=OpenProcess(SYNCHRONIZE,FALSE,pid);
    if(!target){ebon::console::emitRaw("Target process is no longer available.\r\n","warn");return 1;}
    std::uint64_t offset=0;
    std::string pending;
    bool exited=false;
    for(;;){
        HANDLE input=CreateFileW(log.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                                 nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(input!=INVALID_HANDLE_VALUE){
            LARGE_INTEGER size{};
            if(GetFileSizeEx(input,&size)){
                if((std::uint64_t)size.QuadPart<offset){offset=0;pending.clear();}
                LARGE_INTEGER position{};position.QuadPart=offset;
                if(SetFilePointerEx(input,position,nullptr,FILE_BEGIN)){
                    char bytes[8192];DWORD count=0;
                    while(ReadFile(input,bytes,sizeof(bytes),&count,nullptr)&&count){
                        offset+=count;pending.append(bytes,count);
                        std::size_t begin=0,end=0;
                        while((end=pending.find('\n',begin))!=pending.npos){
                            auto line=pending.substr(begin,end+1-begin);
                            const char* level="info";
                            if(line.find("[ebon] success")!=line.npos)level="success";
                            else if(line.find("[ebon] error")!=line.npos)level="error";
                            else if(line.find("[ebon] warn")!=line.npos)level="warn";
                            ebon::console::emitRaw(line,level);
                            begin=end+1;
                        }
                        pending.erase(0,begin);
                    }
                }
            }
            CloseHandle(input);
        }
        if(exited)break; // One final drain after process termination.
        exited=WaitForSingleObject(target,100)==WAIT_OBJECT_0;
    }
    CloseHandle(target);
    return 0;
}
