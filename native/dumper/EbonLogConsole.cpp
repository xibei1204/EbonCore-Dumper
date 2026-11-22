// Separate console owner: the game never allocates or attaches a console.
// It also owns F5 (SDK) and F9 (AES): keys are read from this console's own input
// queue, so it only fires while this window is the active one and the game
// process never polls the keyboard. A press queues a request file that the
// injected worker consumes.
#include "../common/ebon_console.hpp"

namespace {

struct ConsoleInput {
    HANDLE handle=INVALID_HANDLE_VALUE;
    DWORD mode=0;
    bool active=false;
    bool sdkDown=false;
    bool aesDown=false;
};

// Read keys directly, but let the console host handle mouse scrolling and
// Quick Edit selection/copy instead of consuming and discarding mouse events.
bool openConsoleInput(ConsoleInput& input) {
    input.handle=CreateFileW(L"CONIN$",GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,
                             nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(input.handle==INVALID_HANDLE_VALUE)return false;
    if(!GetConsoleMode(input.handle,&input.mode)){CloseHandle(input.handle);input.handle=INVALID_HANDLE_VALUE;return false;}
    const DWORD wanted=(input.mode&~(ENABLE_PROCESSED_INPUT|ENABLE_LINE_INPUT|ENABLE_ECHO_INPUT|
                                    ENABLE_VIRTUAL_TERMINAL_INPUT|ENABLE_MOUSE_INPUT))|
                       ENABLE_EXTENDED_FLAGS|ENABLE_QUICK_EDIT_MODE;
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
        if(record.EventType==FOCUS_EVENT&&!record.Event.FocusEvent.bSetFocus){
            input.sdkDown=input.aesDown=false;
        }
        if(record.EventType!=KEY_EVENT)continue;
        const auto& key=record.Event.KeyEvent;
        if(key.bKeyDown&&key.uChar.AsciiChar==3){quit=true;break;}
        const bool sdk=key.wVirtualKeyCode==VK_F5;
        if(!sdk&&key.wVirtualKeyCode!=VK_F9)continue;
        auto& down=sdk?input.sdkDown:input.aesDown;
        const bool pressed=key.bKeyDown&&!down;
        down=key.bKeyDown!=FALSE;
        if(!pressed||requestDir.empty())continue;
        try {
            std::filesystem::create_directories(requestDir);
            ebon::save(requestDir/(sdk?"sdk_dump.request":"aes_hunt.request"),"");
            ++queued;
            ebon::console::emitRaw(sdk?"F5 queued for Dump SDK.\r\n":"F9 queued for the AES key hunt.\r\n","success");
        } catch(const std::exception& e) {
            ebon::console::emitRaw(std::string(sdk?"F5":"F9")+" request failed: "+e.what()+"\r\n","error");
        }
    }
    return quit;
}

} // namespace

static void startSdkPublisher(const std::filesystem::path& capture,const std::filesystem::path& output,
                              const std::filesystem::path& log,DWORD pid){
    wchar_t executable[32768]{},python[32768]{};
    const auto length=GetModuleFileNameW(nullptr,executable,32768);
    if(!length||length>=32768)return;
    const auto root=std::filesystem::path(executable).parent_path().parent_path();
    const auto script=root/"tools"/"watch_sdk.py";
    const auto found=SearchPathW(nullptr,L"python.exe",nullptr,32768,python,nullptr);
    if(!std::filesystem::is_regular_file(script)||!found||found>=32768){
        ebon::console::emitRaw("F5 C++ SDK publisher unavailable: tools/watch_sdk.py or Python is missing.\r\n","error");return;
    }
    std::wstring command=L"\""+std::wstring(python)+L"\" -u \""+script.wstring()+L"\" --capture \""+capture.wstring()+
        L"\" --out \""+output.wstring()+L"\" --log \""+log.wstring()+L"\" --pid "+std::to_wstring(pid)+
        L" --owner-pid "+std::to_wstring(GetCurrentProcessId());
    STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;
    PROCESS_INFORMATION process{};
    if(!CreateProcessW(python,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW|CREATE_UNICODE_ENVIRONMENT,
                       nullptr,root.c_str(),&startup,&process)){
        ebon::console::emitRaw("F5 C++ SDK publisher could not start, Win32="+std::to_string(GetLastError())+"\r\n","error");return;
    }
    CloseHandle(process.hThread);CloseHandle(process.hProcess);
    ebon::console::emitRaw("F5 C++ SDK body output: "+output.u8string()+"\r\n");
}

int wmain(int argc,wchar_t** argv) {
    std::filesystem::path log,requestDir,sdkOutput;
    DWORD pid=0;
    for(int i=1;i+1<argc;i+=2){
        const std::wstring option=argv[i];
        if(option==L"--log")log=argv[i+1];
        else if(option==L"--pid")pid=wcstoul(argv[i+1],nullptr,10);
        else if(option==L"--request-dir")requestDir=argv[i+1];
        else if(option==L"--sdk-output")sdkOutput=argv[i+1];
        else return 2;
    }
    if(log.empty()||!pid)return 2;
    const auto title=L"EbonCore Dumper - game PID "+std::to_wstring(pid);
    if(!ebon::console::initialize(title.c_str()))return 1;
    ebon::console::emitRaw("EbonCore Dumper | independent log console\r\nClosing this window leaves the game running.\r\n\r\n");
    HANDLE target=OpenProcess(SYNCHRONIZE,FALSE,pid);
    if(!target){ebon::console::emitRaw("Target process is no longer available.\r\n","warn");return 1;}
    if(!requestDir.empty()&&!sdkOutput.empty())startSdkPublisher(requestDir,sdkOutput,log,pid);
    ConsoleInput hotkey;
    const bool inputReady=openConsoleInput(hotkey);
    const bool hotkeyReady=!requestDir.empty()&&inputReady;
    if(!inputReady)
        ebon::console::emitRaw("Console input unavailable: mouse selection and scrolling use the host defaults.\r\n","warn");
    if(!requestDir.empty()&&!hotkeyReady)
        ebon::console::emitRaw("F5/F9 hotkeys unavailable: cannot open this console's input queue.\r\n","warn");
    else if(hotkeyReady)
        ebon::console::emitRaw("F5: Dump SDK from this window.\r\nF9: start the AES key hunt from this window.\r\n");
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
                            const std::string f8Marker="[F8] status=";
                            const auto f8=line.find(f8Marker);
                            if(f8!=line.npos){
                                const auto start=f8+f8Marker.size(),stop=line.find_first_of(" |\r\n",start);
                                const auto state=line.substr(start,stop-start);
                                const std::string label=state=="initializing"?"初始化中，请等待":state=="ready_press_f8"?"已就绪，请按 F8":
                                    state=="armed"?"采样中，等待目标调用":state=="capturing"?"采样中，已收到样本":
                                    state=="window_expired"?"采样窗口已结束":"不可用："+state;
                                SetConsoleTitleW((title+L" | F8: "+ebon::wide(label)).c_str());
                            }
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
