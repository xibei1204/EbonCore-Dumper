// Separate console owner: the game never allocates or attaches a console.
// It also owns the F9 hotkey: the key is read from this console's own input
// queue, so it only fires while this window is the active one and the game
// process never polls the keyboard. A press queues a request file that the
// injected worker consumes.
#include "../common/ebon_console.hpp"

namespace {

struct ConsoleInput {
    HANDLE handle=INVALID_HANDLE_VALUE;
    DWORD mode=0;
    bool active=false;
};

// Keep window input, drop line input/echo/processed keys so a single F9 press
// arrives as an ordinary KEY_EVENT instead of being swallowed by the console.
bool openConsoleInput(ConsoleInput& input) {
    input.handle=CreateFileW(L"CONIN$",GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,
                             nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(input.handle==INVALID_HANDLE_VALUE)return false;
    if(!GetConsoleMode(input.handle,&input.mode)){CloseHandle(input.handle);input.handle=INVALID_HANDLE_VALUE;return false;}
    const DWORD wanted=(input.mode&~(ENABLE_PROCESSED_INPUT|ENABLE_LINE_INPUT|ENABLE_ECHO_INPUT))|ENABLE_EXTENDED_FLAGS;
    if(!SetConsoleMode(input.handle,wanted)){CloseHandle(input.handle);input.handle=INVALID_HANDLE_VALUE;return false;}
    input.active=true;
    return true;
}

void closeConsoleInput(ConsoleInput& input) {
    if(input.handle==INVALID_HANDLE_VALUE)return;
    if(input.active)SetConsoleMode(input.handle,input.mode);
    CloseHandle(input.handle);
    input.handle=INVALID_HANDLE_VALUE;
    input.active=false;
}

// Returns true when the viewer should stop (Ctrl+C, since processed input is off).
bool drainInput(ConsoleInput& input,const std::filesystem::path& requestDir,unsigned& queued) {
    bool quit=false;
    for(;;) {
        INPUT_RECORD record{};
        DWORD count=0;
        if(!PeekConsoleInputW(input.handle,&record,1,&count)||count==0)break;
        if(!ReadConsoleInputW(input.handle,&record,1,&count))break;
        if(record.EventType!=KEY_EVENT||!record.Event.KeyEvent.bKeyDown)continue;
        if(record.Event.KeyEvent.uChar.AsciiChar==3){quit=true;break;}
        if(record.Event.KeyEvent.wVirtualKeyCode!=VK_F9)continue;
        try {
            std::filesystem::create_directories(requestDir);
            ebon::save(requestDir/"aes_hunt.request","");
            ++queued;
            ebon::console::emitRaw("F9 queued for the AES key hunt.\r\n","success");
        } catch(const std::exception& e) {
            ebon::console::emitRaw(std::string("F9 request failed: ")+e.what()+"\r\n","error");
        }
    }
    return quit;
}

} // namespace

int wmain(int argc,wchar_t** argv) {
    std::filesystem::path log,requestDir;
    DWORD pid=0;
    for(int i=1;i+1<argc;i+=2){
        const std::wstring option=argv[i];
        if(option==L"--log")log=argv[i+1];
        else if(option==L"--pid")pid=wcstoul(argv[i+1],nullptr,10);
        else if(option==L"--request-dir")requestDir=argv[i+1];
        else return 2;
    }
    if(log.empty()||!pid)return 2;
    const auto title=L"EbonCore Dumper - game PID "+std::to_wstring(pid);
    if(!ebon::console::initialize(title.c_str()))return 1;
    ebon::console::emitRaw("EbonCore Dumper | independent log console\r\nClosing this window leaves the game running.\r\n\r\n");
    HANDLE target=OpenProcess(SYNCHRONIZE,FALSE,pid);
    if(!target){ebon::console::emitRaw("Target process is no longer available.\r\n","warn");return 1;}
    ConsoleInput hotkey;
    const bool hotkeyReady=!requestDir.empty()&&openConsoleInput(hotkey);
    if(!requestDir.empty()&&!hotkeyReady)
        ebon::console::emitRaw("F9 hotkey unavailable: cannot open this console's input queue.\r\n","warn");
    else if(hotkeyReady)
        ebon::console::emitRaw("F9: start the AES key hunt from this window.\r\n");
    std::uint64_t offset=0;
    unsigned queued=0;
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
        HANDLE waits[2]={target,hotkey.active?hotkey.handle:nullptr};
        const DWORD status=WaitForMultipleObjects(hotkey.active?2:1,waits,FALSE,100);
        if(status==WAIT_OBJECT_0)exited=true;
        else if(status==WAIT_OBJECT_0+1&&drainInput(hotkey,requestDir,queued))break;
        else if(status==WAIT_FAILED)exited=WaitForSingleObject(target,100)==WAIT_OBJECT_0;
    }
    closeConsoleInput(hotkey);
    CloseHandle(target);
    return 0;
}
