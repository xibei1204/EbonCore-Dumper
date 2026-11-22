#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <tlhelp32.h>
#include <shellapi.h>
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "bcrypt.lib")
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <array>
#include <unordered_map>

namespace ebon {
inline std::string utf8(const std::wstring& value) {
    if(value.empty()) return {};
    int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),(int)value.size(),nullptr,0,nullptr,nullptr);
    if(!n) throw std::runtime_error("invalid UTF-16 string");
    std::string result(n,'\0');
    if(!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),(int)value.size(),result.data(),n,nullptr,nullptr)) throw std::runtime_error("UTF-8 conversion failed");
    return result;
}
inline std::wstring wide(const std::string& value) {
    if(value.empty()) return {};
    int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),(int)value.size(),nullptr,0);
    if(!n) throw std::runtime_error("invalid UTF-8 string");
    std::wstring result(n,L'\0'); MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),(int)value.size(),result.data(),n); return result;
}
inline std::string hex(std::uint64_t n) { std::ostringstream s;s<<"0x"<<std::hex<<n;return s.str(); }
inline std::string quote(const std::string& value) {
    std::ostringstream s;s<<'"';
    for(unsigned char c:value) { if(c=='"'||c=='\\')s<<'\\'<<(char)c;else if(c<0x20)s<<"\\u"<<std::hex<<std::setw(4)<<std::setfill('0')<<(int)c<<std::dec;else s<<(char)c; }
    s<<'"';return s.str();
}
inline std::string trim(std::string s) { auto a=s.find_first_not_of(" \t\r\n");if(a==s.npos)return {};auto b=s.find_last_not_of(" \t\r\n");return s.substr(a,b-a+1); }
inline bool keyPressedEdge(SHORT state,bool& wasDown) {
    const bool down=(state&0x8000)!=0;
    const bool pressed=(down&&!wasDown)||(state&0x0001)!=0;
    wasDown=down;
    return pressed;
}
inline bool isForegroundForGame(DWORD foregroundPid,DWORD gamePid,DWORD logViewerPid) {
    return foregroundPid==gamePid||(logViewerPid&&foregroundPid==logViewerPid);
}
struct Config {
    std::map<std::string,std::string> values;
    void load(const std::filesystem::path& path) {
        std::ifstream f(path,std::ios::binary);if(!f)throw std::runtime_error("cannot read profile: "+path.u8string());
        std::string line;int number=0;
        while(std::getline(f,line)) { ++number;if(number==1&&line.compare(0,3,"\xEF\xBB\xBF")==0)line.erase(0,3);line=trim(line);if(line.empty()||line[0]=='#'||line[0]==';'||line[0]=='[')continue;
            auto eq=line.find('=');if(eq==line.npos)throw std::runtime_error("invalid profile line "+std::to_string(number));
            auto key=trim(line.substr(0,eq)),value=trim(line.substr(eq+1)); if(key.empty()||values.count(key))throw std::runtime_error("empty or duplicate profile key: "+key); values[key]=value;
        }
        if(f.bad())throw std::runtime_error("profile read failed");
    }
    std::string text(const std::string& key,const std::string& fallback="")const {auto i=values.find(key);return i==values.end()?fallback:i->second;}
    std::int64_t number(const std::string& key,std::int64_t fallback)const {
        auto i=values.find(key);if(i==values.end())return fallback;try {std::size_t n=0;auto v=std::stoll(i->second,&n,0);if(n!=i->second.size())throw std::runtime_error("suffix");return v;}catch(...){throw std::runtime_error("invalid integer profile key: "+key);}
    }
};
struct Region { std::uint64_t address=0,size=0;bool executable=false,writable=false; };
struct Module { std::uint64_t base=0,size=0;std::wstring name,path; };
inline bool readableProtection(DWORD p) { if(p&(PAGE_NOACCESS|PAGE_GUARD))return false;auto v=p&0xff;return v==PAGE_READONLY||v==PAGE_READWRITE||v==PAGE_WRITECOPY||v==PAGE_EXECUTE_READ||v==PAGE_EXECUTE_READWRITE||v==PAGE_EXECUTE_WRITECOPY; }
class MemoryReader {
public:
    virtual ~MemoryReader()=default;
    virtual bool copy(std::uint64_t address,void* out,std::size_t size)const=0;
    virtual std::vector<Region> regions(const Module& module)const=0;
    virtual bool executable(std::uint64_t address)const=0;
    virtual void refresh()const {}
    template<class T> bool get(std::uint64_t a,T& out)const {return copy(a,&out,sizeof(out));}
};
// Copy each metadata page once per capture pass. All reads still go through
// the underlying reader's protection checks; cached bytes are private copies.
class SnapshotReader final:public MemoryReader {
    const MemoryReader& source_;
    static constexpr std::uint64_t pageSize=4096;
    static constexpr std::size_t maxPages=65536; // Bound cached metadata to 256 MiB.
    mutable std::unordered_map<std::uint64_t,std::array<std::uint8_t,pageSize>> pages_;
    mutable std::unordered_map<std::uint64_t,bool> executable_;
public:
    explicit SnapshotReader(const MemoryReader& source):source_(source){}
    void reset(){pages_.clear();executable_.clear();}
    void refresh()const override {pages_.clear();executable_.clear();}
    bool copy(std::uint64_t address,void* output,std::size_t size)const override {
        if(!output||!size||address<0x10000||address+size<address)return false;
        if(size>pageSize)return source_.copy(address,output,size);
        auto* target=static_cast<std::uint8_t*>(output);
        while(size){
            const auto page=address&~(pageSize-1),offset=address-page;
            const auto count=std::min<std::size_t>(size,(std::size_t)(pageSize-offset));
            auto it=pages_.find(page);
            if(it==pages_.end()&&pages_.size()<maxPages){
                std::array<std::uint8_t,pageSize> bytes{};
                if(source_.copy(page,bytes.data(),bytes.size()))it=pages_.emplace(page,std::move(bytes)).first;
            }
            if(it!=pages_.end())std::memcpy(target,it->second.data()+offset,count);
            else if(!source_.copy(address,target,count))return false;
            address+=count;target+=count;size-=count;
        }
        return true;
    }
    std::vector<Region> regions(const Module& module)const override{return source_.regions(module);}
    bool executable(std::uint64_t address)const override {
        if(address<0x10000)return false;
        const auto page=address&~(pageSize-1);
        const auto it=executable_.find(page);
        if(it!=executable_.end())return it->second;
        const bool result=source_.executable(address);
        if(executable_.size()<maxPages)executable_[page]=result;
        return result;
    }
};
class ProcessReader final:public MemoryReader {
    HANDLE process_=nullptr;
public:
    explicit ProcessReader(DWORD pid) {process_=OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ,FALSE,pid);if(!process_)throw std::runtime_error("OpenProcess read-only failed, Win32="+std::to_string(GetLastError()));}
    ~ProcessReader(){if(process_)CloseHandle(process_);}
    ProcessReader(const ProcessReader&)=delete;ProcessReader& operator=(const ProcessReader&)=delete;
    bool copy(std::uint64_t a,void* out,std::size_t size)const override {
        if(!out||!size||a<0x10000||a+size<a)return false;
        std::uint64_t at=a,end=a+size;
        while(at<end){MEMORY_BASIC_INFORMATION v{};if(!VirtualQueryEx(process_,(void*)at,&v,sizeof(v))||v.State!=MEM_COMMIT||!readableProtection(v.Protect))return false;auto next=(std::uint64_t)v.BaseAddress+v.RegionSize;if(next<=at)return false;at=std::min(next,end);}
        SIZE_T n=0;return ReadProcessMemory(process_,(void*)a,out,size,&n)&&n==size;
    }
    bool executable(std::uint64_t a)const override {MEMORY_BASIC_INFORMATION v{};if(!VirtualQueryEx(process_,(void*)a,&v,sizeof(v))||v.State!=MEM_COMMIT||!readableProtection(v.Protect))return false;auto p=v.Protect&0xff;return p==PAGE_EXECUTE_READ||p==PAGE_EXECUTE_READWRITE||p==PAGE_EXECUTE_WRITECOPY;}
    std::vector<Region> regions(const Module& m)const override {
        std::vector<Region> result;std::uint64_t at=m.base,end=m.base+m.size;
        while(at<end){MEMORY_BASIC_INFORMATION v{};if(!VirtualQueryEx(process_,(void*)at,&v,sizeof(v)))break;auto next=(std::uint64_t)v.BaseAddress+v.RegionSize;if(next<=at)break;
            if(v.State==MEM_COMMIT&&readableProtection(v.Protect)){auto p=v.Protect&0xff;bool x=p==PAGE_EXECUTE_READ||p==PAGE_EXECUTE_READWRITE||p==PAGE_EXECUTE_WRITECOPY;bool w=p==PAGE_READWRITE||p==PAGE_WRITECOPY||p==PAGE_EXECUTE_READWRITE||p==PAGE_EXECUTE_WRITECOPY;result.push_back({at,std::min(next,end)-at,x,w});}at=next;}
        return result;
    }
};
class InProcessReader final:public MemoryReader {
public:
    InProcessReader()=default;
    bool copy(std::uint64_t a,void* out,std::size_t size)const override {
        if(!out||!size||a<0x10000||a+size<a)return false;
        std::uint64_t at=a,end=a+size;
        while(at<end){MEMORY_BASIC_INFORMATION v{};if(!VirtualQuery((void*)at,&v,sizeof(v))||v.State!=MEM_COMMIT||!readableProtection(v.Protect))return false;auto next=(std::uint64_t)v.BaseAddress+v.RegionSize;if(next<=at)return false;at=std::min(next,end);}
        __try {
            std::memcpy(out,(const void*)a,size);
            return true;
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }
    bool executable(std::uint64_t a)const override {MEMORY_BASIC_INFORMATION v{};if(!VirtualQuery((void*)a,&v,sizeof(v))||v.State!=MEM_COMMIT||!readableProtection(v.Protect))return false;auto p=v.Protect&0xff;return p==PAGE_EXECUTE_READ||p==PAGE_EXECUTE_READWRITE||p==PAGE_EXECUTE_WRITECOPY;}
    std::vector<Region> regions(const Module& m)const override {
        std::vector<Region> result;std::uint64_t at=m.base,end=m.base+m.size;
        while(at<end){MEMORY_BASIC_INFORMATION v{};if(!VirtualQuery((void*)at,&v,sizeof(v)))break;auto next=(std::uint64_t)v.BaseAddress+v.RegionSize;if(next<=at)break;
            if(v.State==MEM_COMMIT&&readableProtection(v.Protect)){auto p=v.Protect&0xff;bool x=p==PAGE_EXECUTE_READ||p==PAGE_EXECUTE_READWRITE||p==PAGE_EXECUTE_WRITECOPY;bool w=p==PAGE_READWRITE||p==PAGE_WRITECOPY||p==PAGE_EXECUTE_READWRITE||p==PAGE_EXECUTE_WRITECOPY;result.push_back({at,std::min(next,end)-at,x,w});}at=next;}
        return result;
    }
};
inline Module findModule(DWORD pid,const std::wstring& name) {
    if(pid==GetCurrentProcessId()){
        HMODULE hMod=GetModuleHandleW(name.empty()?nullptr:name.c_str());
        if(hMod){
            wchar_t path[MAX_PATH]={};
            GetModuleFileNameW(hMod,path,MAX_PATH);
            auto dos=reinterpret_cast<PIMAGE_DOS_HEADER>(hMod);
            std::uint64_t size=0;
            if(dos->e_magic==IMAGE_DOS_SIGNATURE){
                auto nt=reinterpret_cast<PIMAGE_NT_HEADERS>(reinterpret_cast<std::uint8_t*>(hMod)+dos->e_lfanew);
                if(nt->Signature==IMAGE_NT_SIGNATURE){size=nt->OptionalHeader.SizeOfImage;}
            }
            const wchar_t* baseName=wcsrchr(path,L'\\');
            return {(std::uint64_t)hMod,size,baseName?baseName+1:path,path};
        }
    }
    HANDLE h=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,pid);if(h==INVALID_HANDLE_VALUE)throw std::runtime_error("module snapshot failed, Win32="+std::to_string(GetLastError()));
    MODULEENTRY32W v{};v.dwSize=sizeof(v);Module result;
    if(Module32FirstW(h,&v))do{if(name.empty()||!_wcsicmp(name.c_str(),v.szModule)){result={(std::uint64_t)v.modBaseAddr,v.modBaseSize,v.szModule,v.szExePath};break;}}while(Module32NextW(h,&v));CloseHandle(h);
    if(!result.base)throw std::runtime_error("requested module not found: "+utf8(name));return result;
}
inline void save(const std::filesystem::path& file,const std::string& value) {std::ofstream f(file,std::ios::binary|std::ios::trunc);if(!f||!(f<<value)||!f.flush())throw std::runtime_error("cannot write "+file.u8string());}
inline std::string sha256File(const std::wstring& path) {
    BCRYPT_ALG_HANDLE alg=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;PUCHAR object=nullptr;
    auto fail=[&](NTSTATUS status,const char* operation){if(hash)BCryptDestroyHash(hash);if(alg)BCryptCloseAlgorithmProvider(alg,0);if(object)HeapFree(GetProcessHeap(),0,object);throw std::runtime_error(std::string(operation)+" failed, NTSTATUS="+std::to_string((unsigned long)status));};
    NTSTATUS st=BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,nullptr,0);if(st<0)fail(st,"BCryptOpenAlgorithmProvider");DWORD objectBytes=0,got=0;
    st=BCryptGetProperty(alg,BCRYPT_OBJECT_LENGTH,(PUCHAR)&objectBytes,sizeof(objectBytes),&got,0);if(st<0)fail(st,"BCryptGetProperty");object=(PUCHAR)HeapAlloc(GetProcessHeap(),0,objectBytes);if(!object)fail((NTSTATUS)0xC0000017,"HeapAlloc");
    st=BCryptCreateHash(alg,&hash,object,objectBytes,nullptr,0,0);if(st<0)fail(st,"BCryptCreateHash");std::ifstream input(std::filesystem::path(path),std::ios::binary);if(!input)fail((NTSTATUS)0xC0000034,"open target module");
    std::vector<UCHAR> buffer(1024*1024);while(input){input.read((char*)buffer.data(),buffer.size());auto n=input.gcount();if(n>0){st=BCryptHashData(hash,buffer.data(),(ULONG)n,0);if(st<0)fail(st,"BCryptHashData");}}if(input.bad())fail((NTSTATUS)0xC0000185,"read target module");
    UCHAR digest[32]{};st=BCryptFinishHash(hash,digest,sizeof(digest),0);if(st<0)fail(st,"BCryptFinishHash");static const char digits[]="0123456789abcdef";std::string result;for(auto b:digest){result+=digits[b>>4];result+=digits[b&15];}
    BCryptDestroyHash(hash);BCryptCloseAlgorithmProvider(alg,0);HeapFree(GetProcessHeap(),0,object);return result;
}
inline bool isElevated() {
    HANDLE tok = nullptr;
    if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&tok)) return false;
    TOKEN_ELEVATION el{};
    DWORD sz = sizeof(el);
    bool ok = false;
    if(GetTokenInformation(tok,TokenElevation,&el,sizeof(el),&sz))
        ok = (el.TokenIsElevated != 0);
    CloseHandle(tok);
    return ok;
}
inline bool enableDebugPrivilege() {
    HANDLE tok = nullptr;
    if(!OpenProcessToken(GetCurrentProcess(),TOKEN_ADJUST_PRIVILEGES|TOKEN_QUERY,&tok)) return false;
    LUID luid{};
    if(!LookupPrivilegeValueW(nullptr,SE_DEBUG_NAME,&luid)){CloseHandle(tok);return false;}
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    BOOL ok = AdjustTokenPrivileges(tok,FALSE,&tp,sizeof(tp),nullptr,nullptr);
    DWORD err = GetLastError();
    CloseHandle(tok);
    return ok && (err != ERROR_NOT_ALL_ASSIGNED);
}
inline bool relaunchSelfElevated() {
    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring all = GetCommandLineW();
    std::wstring tail = all;
    if(!tail.empty() && tail[0] == L'"') {
        size_t e = tail.find(L'"', 1);
        if(e != std::wstring::npos) tail = tail.substr(e + 1);
    } else {
        size_t sp = tail.find(L' ');
        tail = (sp == std::wstring::npos) ? std::wstring() : tail.substr(sp);
    }
    size_t nonSpace = tail.find_first_not_of(L' ');
    if(nonSpace != std::wstring::npos) tail = tail.substr(nonSpace);
    else tail.clear();

    wchar_t cwd[MAX_PATH] = {};
    GetCurrentDirectoryW(MAX_PATH, cwd);
    SHELLEXECUTEINFOW si{};
    si.cbSize = sizeof(si);
    si.fMask = SEE_MASK_NOCLOSEPROCESS;
    si.lpVerb = L"runas";
    si.lpFile = self;
    si.lpParameters = tail.c_str();
    si.lpDirectory = cwd;
    si.nShow = SW_SHOWNORMAL;
    if(!ShellExecuteExW(&si)) return false;
    if(si.hProcess) {
        WaitForSingleObject(si.hProcess, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(si.hProcess, &code);
        CloseHandle(si.hProcess);
        ExitProcess(code);
    }
    return true;
}
} // namespace ebon
