#include "../sdk/ebon_reflection.hpp"
#include "../common/ebon_console.hpp"
#include "ebon_aes_hunter.hpp"
#include "ebon_resource_dump.hpp"
#include "../sdk/ebon_network_version.hpp"
#include <memory>
#include <chrono>

static int dump(DWORD pid,const std::filesystem::path& profile,const std::filesystem::path& output) {
    try {
        ebon::Config config;
        config.load(profile);
        std::unique_ptr<ebon::MemoryReader> reader;
        if(pid==GetCurrentProcessId()){
            reader=std::make_unique<ebon::InProcessReader>();
        } else {
            reader=std::make_unique<ebon::ProcessReader>(pid);
        }
        auto module=ebon::findModule(pid,ebon::wide(config.text("module","HTGame.exe")));
        auto digest=ebon::sha256File(module.path);
        auto expected=config.text("exe_sha256");
        std::transform(expected.begin(),expected.end(),expected.begin(),[](unsigned char c){return (char)std::tolower(c);});
        if(!expected.empty()&&expected!=digest)
            throw std::runtime_error("module SHA-256 mismatch; profile does not match the running build");
        ebon::SnapshotReader snapshot(*reader);
        ebon::Reflection reflection(snapshot,config,module);
        reflection.moduleSha256=digest;
        bool ok=reflection.collect();
        reflection.write(output,ok);
        ebon::saveNetworkVersion(output,ebon::observeNetworkVersion(*reader,config,module,digest));
        std::string summary="objects="+std::to_string(reflection.objects.size())+" types="+std::to_string(reflection.types.size())+" properties="+std::to_string(reflection.propertyCount)+" success="+std::to_string(ok?1:0)+"\n";
        std::cout<<summary;
        OutputDebugStringA(("[ebon_dumper] "+summary).c_str());
        for(const auto& error:reflection.errors){
            std::cerr<<error<<"\n";
            OutputDebugStringA(("[ebon_dumper] error: "+error+"\n").c_str());
        }
        return ok?0:1;
    }
    catch(const std::exception& e){
        std::cerr<<e.what()<<"\n";
        OutputDebugStringA(("[ebon_dumper] exception: "+std::string(e.what())+"\n").c_str());
        try{
            std::filesystem::create_directories(output);
            ebon::save(output/"dump_state.json","{\"format\":\"EbonReflection-v1\",\"status\":\"failed\",\"success\":false,\"errors\":["+ebon::quote(e.what())+"]}\n");
        }catch(...){}
        return 1;
    }
}

#ifndef EBON_DLL
static int dumpNetworkVersion(DWORD pid,const std::filesystem::path& profile,const std::filesystem::path& output){
    ebon::Config config;config.load(profile);
    std::unique_ptr<ebon::MemoryReader> reader;
    if(pid==GetCurrentProcessId())reader=std::make_unique<ebon::InProcessReader>();
    else reader=std::make_unique<ebon::ProcessReader>(pid);
    const auto module=ebon::findModule(pid,ebon::wide(config.text("module","HTGame.exe")));
    const auto digest=ebon::sha256File(module.path);
    const auto version=ebon::observeNetworkVersion(*reader,config,module,digest);
    ebon::saveNetworkVersion(output,version);
    ebon::save(output/"dump_state.json","{\"format\":\"EbonNetworkVersionCapture-v1\",\"build\":{\"sha256\":"+
        ebon::quote(digest)+"},\"module_base\":"+ebon::quote(ebon::hex(module.base))+",\"reflection_captured\":false}\n");
    std::cout<<version.json();return version.value?0:1;
}
#endif

static void validateOutput(const std::filesystem::path& profile,const std::filesystem::path& output) {
    ebon::Config config;config.load(profile);
    const auto rootText=config.text("client_root");
    if(rootText.empty())throw std::runtime_error("profile must define client_root to protect the installation");
    auto root=std::filesystem::weakly_canonical(std::filesystem::path(ebon::wide(rootText))).wstring();
    auto target=std::filesystem::weakly_canonical(output).wstring();
    while(!root.empty()&&(root.back()==L'\\'||root.back()==L'/'))root.pop_back();
    if(target.size()>=root.size()&&CompareStringOrdinal(target.data(),(int)root.size(),root.data(),(int)root.size(),TRUE)==CSTR_EQUAL&&
       (target.size()==root.size()||target[root.size()]==L'\\'||target[root.size()]==L'/'))
        throw std::runtime_error("output must be outside the read-only client installation");
    if(std::filesystem::exists(output)) {
        if(!std::filesystem::is_directory(output)||std::filesystem::directory_iterator(output)!=std::filesystem::directory_iterator{})
            throw std::runtime_error("output directory must be new or empty");
    }
}

#ifdef EBON_DLL
#pragma comment(lib, "user32.lib")
static HMODULE selfModule=nullptr;

static void captureResources(const ebon::Config& config,const ebon::Module& module,
                             const std::string& digest,const std::filesystem::path& output) {
    bool outputOwned=false;
    try {
        std::filesystem::create_directories(output.parent_path());
        if(!std::filesystem::create_directory(output))throw std::runtime_error("resource capture directory already exists");
        outputOwned=true;
        ebon::save(output/"resource_state.json",
            "{\"format\":\"EbonResources-v1\",\"status\":\"discovering\",\"success\":false}\n");
        ebon::InProcessReader memory;
        auto resourceConfig=config;
        // Capture the menu component cache in the same fresh inventory as its
        // instance and tables. This also avoids pairing different sessions.
        resourceConfig.values["collect_network"]=config.number("resource_capture_network",1)?"1":"0";
        const auto timeout=config.number("resource_timeout_seconds",180);
        if(timeout<1||timeout>1800)throw std::runtime_error("resource_timeout_seconds must be in 1..1800");
        ebon::console::logf("info","F10 resource capture started: %s",output.u8string().c_str());
        const auto deadline=GetTickCount64()+(ULONGLONG)timeout*1000;
        std::unique_ptr<ebon::SnapshotReader> metadata;
        std::unique_ptr<ebon::Reflection> current;
        bool reflectionClean=false;
        std::vector<std::string> attemptDiagnostics;
        for(unsigned attempt=1;attempt<=3;++attempt){
            if(GetTickCount64()>=deadline)break;
            metadata=std::make_unique<ebon::SnapshotReader>(memory);
            current=std::make_unique<ebon::Reflection>(*metadata,resourceConfig,module);
            current->moduleSha256=digest;
            current->setDeadline(deadline);
            try{reflectionClean=current->collect();}
            catch(const std::exception& e){
                const auto detail="attempt "+std::to_string(attempt)+" threw during live reflection: "+e.what();
                attemptDiagnostics.push_back(detail);ebon::console::log("warn","resource reflection retry: "+detail);
                current.reset();metadata.reset();if(attempt<3&&GetTickCount64()<deadline)Sleep(500);continue;
            }
            if(reflectionClean||current->resourceInventoryUsable()){
                if(!reflectionClean){
                    std::string detail="partial validated inventory after attempt "+std::to_string(attempt)+
                        ": objects="+std::to_string(current->objects.size())+" types="+std::to_string(current->types.size())+
                        " properties="+std::to_string(current->propertyCount)+" errors="+std::to_string(current->errors.size());
                    for(std::size_t i=0;i<std::min<std::size_t>(current->errors.size(),8);++i)detail+=" | "+current->errors[i];
                    attemptDiagnostics.push_back(detail);
                    ebon::console::log("warn","fresh reflection has transient errors; exporting with per-object validation: "+detail);
                }
                break;
            }
            std::string detail="attempt "+std::to_string(attempt)+" unusable inventory: objects="+
                std::to_string(current->objects.size())+" types="+std::to_string(current->types.size())+
                " properties="+std::to_string(current->propertyCount)+" errors="+std::to_string(current->errors.size());
            for(std::size_t i=0;i<std::min<std::size_t>(current->errors.size(),8);++i)detail+=" | "+current->errors[i];
            attemptDiagnostics.push_back(detail);
            ebon::console::log("warn","resource reflection retry: "+detail);
            current.reset();metadata.reset();
            if(attempt<3&&GetTickCount64()<deadline)Sleep(500);
        }
        // The large-inventory threshold is for degraded live snapshots only.
        // A clean collect already validated both global and property layouts.
        if(!current||(!reflectionClean&&!current->resourceInventoryUsable())){
            std::string detail="fresh reflection did not produce a validated resource inventory";
            for(const auto& item:attemptDiagnostics)detail+="; "+item;
            ebon::save(output/"reflection_attempts.json","{\"attempts\":["+
                (attemptDiagnostics.empty()?std::string():[&](){std::string s;for(std::size_t i=0;i<attemptDiagnostics.size();++i){if(i)s+=',';s+=ebon::quote(attemptDiagnostics[i]);}return s;}())+
                "]}\n");
            throw std::runtime_error(detail);
        }
        if(!attemptDiagnostics.empty()){
            std::string rows;for(std::size_t i=0;i<attemptDiagnostics.size();++i){if(i)rows+=',';rows+=ebon::quote(attemptDiagnostics[i]);}
            ebon::save(output/"reflection_attempts.json","{\"attempts\":["+rows+"]}\n");
        }
        // Save the exact schemas used to decode values, independently of the
        // original startup snapshot, which may precede scene/asset loading.
        current->write(output/"reflection",reflectionClean);
        if(!reflectionClean)current->writeState(output/"reflection",false,"partial_live_inventory");
        // Keep the owned discovering state: a polling Windows reader may deny
        // DeleteFileW sharing even though it allows the normal state writes.
        ebon::resources::Dumper dumper(memory,*current,resourceConfig,output);
        const bool ok=dumper.run(true);
        ebon::console::logf(ok?"success":"warn","F10 resource capture finished: success=%d state=%s",
            ok?1:0,(output/"resource_state.json").u8string().c_str());
        ebon::console::logf("info","F10 resource bodies: %s",
            std::filesystem::absolute(output/"data").lexically_normal().u8string().c_str());
    } catch(const std::exception& e) {
        ebon::console::logf("error","F10 resource capture failed: %s",e.what());
        if(outputOwned)try {ebon::save(output/"resource_state.json","{\"format\":\"EbonResources-v1\",\"status\":\"failed\",\"success\":false,\"error\":"+ebon::quote(e.what())+"}\n");}catch(...){}
    }
}

// F9 arms the AES index-key hunt. The dumper terminal reads the key from its own
// console input queue and writes aes_hunt.request into this capture, so the
// hotkey only exists inside that terminal window; the game never polls the
// keyboard. The request file is also the scripted trigger.
static const DWORD aesHotkeyPollMs=15;
static const DWORD aesRunCheckMs=250;

// The scan publishes initializing/scanning/raw_* while it works and one of the
// terminal statuses when it stops, so that token is the completion edge.
static bool aesRunActive(const std::filesystem::path& state) {
    std::ifstream input(state,std::ios::binary);
    if(!input)return true; // Dispatched, state not yet published.
    const std::string text((std::istreambuf_iterator<char>(input)),{});
    if(text.empty())return true;
    for(const char* done:{"\"status\":\"verified\"","\"status\":\"exhausted\"","\"status\":\"timed_out\"",
                          "\"status\":\"error\"","\"status\":\"cancelled\""})
        if(text.find(done)!=std::string::npos)return false;
    return true;
}

// The first run keeps <capture>/aes so wait-aes and existing tooling still
// resolve; later presses take aes_2, aes_3, ... because a scan needs its own
// empty directory.
static std::filesystem::path nextAesDirectory(const std::filesystem::path& output,unsigned& serial) {
    std::filesystem::path candidate;
    do {candidate=serial==0?output/"aes":output/("aes_"+std::to_string(serial+1));++serial;}
    while(std::filesystem::exists(candidate));
    return candidate;
}

struct AesHotkeyJob {
    std::filesystem::path output;
    ebon::Config config;
    DWORD terminalPid=0;
    HMODULE module=nullptr;
};

// The thread owns the job and the pinned DLL reference for the process
// lifetime; it never returns while the game runs.
static DWORD WINAPI aesHotkeyWorker(LPVOID param) {
    std::unique_ptr<AesHotkeyJob> job(static_cast<AesHotkeyJob*>(param));
    const auto marker=job->output/"aes_hotkey_state.json";
    const auto request=job->output/"aes_hunt.request";
    const bool terminal=job->terminalPid!=0;
    unsigned presses=0,runs=0,serial=0;
    std::filesystem::path running,lastOutput;
    const auto publish=[&](const char* status) {
        try {
            ebon::save(marker,"{\"format\":\"EbonAesHotkey-v1\",\"status\":"+ebon::quote(status)+
                ",\"key\":\"F9\",\"terminal_pid\":"+std::to_string(job->terminalPid)+
                ",\"terminal_available\":"+(terminal?"true":"false")+
                ",\"presses\":"+std::to_string(presses)+",\"runs\":"+std::to_string(runs)+
                ",\"running\":"+(!running.empty()?"true":"false")+
                ",\"last_output\":"+ebon::quote(lastOutput.empty()?std::string():lastOutput.u8string())+"}\n");
        } catch(const std::exception& e){ebon::console::logf("warn","cannot publish AES hotkey state: %s",e.what());}
    };
    const auto start=[&](const char* source) {
        const auto target=nextAesDirectory(job->output,serial);
        try {
            ebon_hunter::startBackground(ebon_hunter::fromConfig(job->config,target));
            running=target/"aes_state.json";lastOutput=target;++runs;
            ebon::console::logf("success","AES key hunt started from %s; state=%s",source,running.u8string().c_str());
        } catch(const std::exception& e) {
            try {
                std::filesystem::create_directories(target);
                ebon_hunter::atomicSave(target/"aes_state.json",
                    "{\"format\":\"EbonAES-v1\",\"status\":\"error\",\"success\":false,\"error\":"+ebon::quote(e.what())+"}\n");
            } catch(...) {}
            lastOutput=target;
            ebon::console::logf("warn","AES key hunt failed to start: %s",e.what());
        }
    };
    publish("armed");
    ULONGLONG nextRunCheck=0;
    for(;;) {
        if(!running.empty()&&GetTickCount64()>=nextRunCheck) {
            nextRunCheck=GetTickCount64()+aesRunCheckMs;
            if(!aesRunActive(running)) {
                ebon::console::logf("info","AES key hunt stopped; state=%s",running.u8string().c_str());
                running.clear();
                publish("armed");
            }
        }
        const bool requested=GetFileAttributesW(request.c_str())!=INVALID_FILE_ATTRIBUTES;
        if(requested) {
            DeleteFileW(request.c_str());
            if(!running.empty()) {
                ebon::console::logf("warn","AES key hunt already running; ignoring F9");
            } else {
                ++presses;
                start("the dumper terminal");
                publish(running.empty()?"armed":"running");
            }
        }
        Sleep(aesHotkeyPollMs);
    }
}

static void startAesHotkey(const ebon::Config& config,const std::filesystem::path& output,DWORD terminalPid) {
    auto job=std::make_unique<AesHotkeyJob>();
    job->output=output;job->config=config;job->terminalPid=terminalPid;
    HMODULE module=nullptr;
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,reinterpret_cast<LPCWSTR>(&aesHotkeyWorker),&module))
        throw std::runtime_error("cannot retain DLL for the AES hotkey listener");
    job->module=module;
    const auto thread=CreateThread(nullptr,0,aesHotkeyWorker,job.get(),0,nullptr);
    if(!thread) {FreeLibrary(module);throw std::runtime_error("cannot start the AES hotkey listener");}
    job.release();
    CloseHandle(thread);
}

static DWORD WINAPI injectedWorker(LPVOID param) {
    std::filesystem::path outputPath;
    bool outputReserved = false;
    try {
        wchar_t dllPath[32768]{};
        const auto length = GetModuleFileNameW(reinterpret_cast<HMODULE>(param), dllPath, 32768);
        if (!length || length >= 32768) throw std::runtime_error("cannot resolve dumper DLL path");
        const auto dllDir = std::filesystem::path(dllPath).parent_path();
        const auto iniPath = dllDir / "EbonDumper.ini";
        auto logPath = dllDir.parent_path() / "out" / "ebon_inject.log";
        std::filesystem::create_directories(logPath.parent_path());
        ebon::console::setLogFile(logPath);
        ebon::console::logf("info", "injected worker started pid=%lu dll=%s",
                            GetCurrentProcessId(), std::filesystem::path(dllPath).u8string().c_str());

        ebon::Config ini;
        if (std::filesystem::exists(iniPath)) ini.load(iniPath);
        bool autoDump = ini.number("auto_dump", 1) != 0;
        const auto maxWaitSec = ini.number("max_wait_seconds", 180);
        const auto settleSeconds = ini.number("settle_seconds", 2);
        if (maxWaitSec < 1 || maxWaitSec > 86400)
            throw std::runtime_error("max_wait_seconds must be in 1..86400");
        if (settleSeconds < 0 || settleSeconds > 60)
            throw std::runtime_error("settle_seconds must be in 0..60");
        auto profilePath = std::filesystem::path(ebon::wide(ini.text("profile")));
        outputPath = ebon::wide(ini.text("output"));
        if (!ini.text("log").empty()) logPath = ebon::wide(ini.text("log"));
        if (logPath.is_relative()) logPath = dllDir / logPath;
        std::filesystem::create_directories(logPath.parent_path());
        ebon::console::setLogFile(logPath);
        DWORD logViewerPid=0;

        wchar_t env[32768]{};
        const auto environment = [&](const wchar_t* key) -> std::wstring {
            const auto n = GetEnvironmentVariableW(key, env, 32768);
            if (n >= 32768) throw std::runtime_error("environment path is too long");
            return n ? std::wstring(env, n) : std::wstring();
        };
        if (profilePath.empty()) profilePath = environment(L"EBON_PROFILE");
        if (outputPath.empty()) outputPath = environment(L"EBON_OUT");
        const auto autoEnv = environment(L"EBON_AUTO_DUMP");
        if (!autoEnv.empty()) autoDump = _wtoi(autoEnv.c_str()) != 0;
        if (!autoDump) {
            ebon::console::log("warn", "auto_dump is disabled; call EbonDumpW explicitly to capture");
            return 0;
        }
        if (profilePath.empty()) {
            profilePath = dllDir.parent_path() / "profiles" / "nte-ue5.ini";
            if (!std::filesystem::exists(profilePath)) profilePath = dllDir / "profiles" / "nte-ue5.ini";
        } else if (profilePath.is_relative()) profilePath = dllDir / profilePath;
        if (outputPath.empty()) {
            SYSTEMTIME now{};
            GetLocalTime(&now);
            char name[96]{};
            _snprintf_s(name, sizeof(name), _TRUNCATE, "injected_%04u%02u%02u_%02u%02u%02u_%lu",
                        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
                        GetCurrentProcessId());
            outputPath = dllDir.parent_path() / "out" / "reflection" / name;
        } else if (outputPath.is_relative()) outputPath = dllDir / outputPath;

        ebon::Config config;
        config.load(profilePath);
        validateOutput(profilePath, outputPath);
        std::filesystem::create_directories(outputPath);
        outputReserved = true;

        // The terminal is started only after the capture directory exists: it
        // owns the F9 hotkey and queues aes_hunt.request inside that directory.
        if(ini.number("console",1)!=0){
            if(!ebon::console::startViewer(dllDir / "EbonLogConsole.exe",GetCurrentProcessId(),outputPath,&logViewerPid))
                ebon::console::logf("warn","cannot start standalone log console, Win32=%lu; log=%s",GetLastError(),logPath.u8string().c_str());
        }

        ebon::save(outputPath / "dump_state.json",
                   "{\"format\":\"EbonReflection-v1\",\"status\":\"initializing\",\"phase\":\"module_validation\",\"success\":false}\n");
        ebon::console::logf("info", "profile=%s", profilePath.u8string().c_str());
        ebon::console::logf("info", "output =%s", outputPath.u8string().c_str());
        ebon::console::log("info", "validating target module SHA-256");
        const auto mod = ebon::findModule(GetCurrentProcessId(), ebon::wide(config.text("module", "HTGame.exe")));
        const auto digest = ebon::sha256File(mod.path);
        auto expected = config.text("exe_sha256");
        std::transform(expected.begin(), expected.end(), expected.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        if (!expected.empty() && expected != digest)
            throw std::runtime_error("module SHA-256 mismatch; profile does not match the running build");
        ebon::console::logf("success", "module validated: %s sha256=%s", ebon::utf8(mod.name).c_str(), digest.c_str());

        // Start only after the selected profile and output have passed validation.
        // AES has its own lifetime/deadline and never holds up reflection capture.
        // aes_hotkey=1 only listens for F9 inside this capture's dumper terminal,
        // aes_hotkey=0 restores the immediate scan for scripted runs.
        if(config.number("aes_hunt",1)==0) {
            std::filesystem::create_directories(outputPath/"aes");
            ebon_hunter::atomicSave(outputPath/"aes"/"aes_state.json",
                "{\"format\":\"EbonAES-v1\",\"status\":\"disabled\",\"success\":false}\n");
            ebon::console::log("info","AES key hunt disabled by aes_hunt=0");
        } else if(config.number("aes_hotkey",1)!=0) {
            try {
                startAesHotkey(config,outputPath,logViewerPid);
                if(logViewerPid)
                    ebon::console::log("success","F9 AES key hunt armed: press F9 inside this capture's dumper terminal to search the Pak index key. Each run writes aes\\ or aes_N\\");
                else
                    ebon::console::log("warn","F9 AES key hunt armed without a dumper terminal (console=0); create aes_hunt.request in the capture directory to start it");
            } catch(const std::exception& e) {
                ebon::console::logf("warn","AES hotkey listener failed: %s",e.what());
            }
        } else {
            try {
                ebon_hunter::startBackground(ebon_hunter::fromConfig(config,outputPath/"aes"));
                ebon::console::logf("info","AES worker started immediately (aes_hotkey=0); state=%s",(outputPath/"aes"/"aes_state.json").u8string().c_str());
            } catch(const std::exception& e) {
                std::filesystem::create_directories(outputPath/"aes");
                ebon_hunter::atomicSave(outputPath/"aes"/"aes_state.json",
                    "{\"format\":\"EbonAES-v1\",\"status\":\"error\",\"success\":false,\"error\":"+ebon::quote(e.what())+"}\n");
                ebon::console::logf("warn","AES worker failed: %s",e.what());
            }
        }

        ebon::InProcessReader memory;
        ebon::SnapshotReader snapshot(memory);
        ebon::Reflection reflection(snapshot, config, mod);
        reflection.moduleSha256 = digest;
        ebon::NetworkVersionObservation lastVersion;
        bool versionPublished=false;
        const auto updateVersion=[&](const std::filesystem::path& where,bool force){
            const auto version=ebon::observeNetworkVersion(memory,config,mod,digest);
            if(force||!versionPublished||version.status!=lastVersion.status||version.value!=lastVersion.value){
                ebon::saveNetworkVersion(where,version);
                if(where==outputPath){
                    lastVersion=version;versionPublished=true;
                    if(version.value)ebon::console::logf("success","Local NetworkVersion observed: %u (0x%08x); getter=%s",
                        *version.value,*version.value,ebon::hex(version.getterRva).c_str());
                    else ebon::console::logf("info","Local NetworkVersion: %s; no guessed value",version.status.c_str());
                }
            }
        };
        const auto captureDelay=ini.number("capture_delay_seconds",0);
        if(captureDelay<0||captureDelay>300)throw std::runtime_error("capture_delay_seconds must be in 0..300");
        for(auto remaining=captureDelay;remaining>0;--remaining){
            if(remaining==captureDelay||remaining%10==0)ebon::console::logf("info","capture delayed for network initialization: %lld seconds remaining",(long long)remaining);
            reflection.progressPhase="capture_delay";reflection.progressDone=captureDelay-remaining;reflection.progressTotal=captureDelay;
            reflection.writeState(outputPath,false,"waiting");
            Sleep(1000);
        }
        const auto deadline = GetTickCount64() + (ULONGLONG)maxWaitSec * 1000;
        reflection.setDeadline(deadline);
        ULONGLONG lastProgress = 0;
        std::string lastPhase;
        reflection.onProgress = [&](const char* phase, std::uint64_t done, std::uint64_t total) {
            const auto now = GetTickCount64();
            if (phase == lastPhase && now - lastProgress < 1000) return;
            lastPhase = phase;
            lastProgress = now;
            const bool scanning = std::string(phase).find("scan") != std::string::npos;
            ebon::console::logf("info", "%s: %llu/%llu %s; name pools=%zu object arrays=%zu",
                                phase, (unsigned long long)done, (unsigned long long)total,
                                scanning ? "bytes" : "objects", reflection.poolCandidates.size(),
                                reflection.arrayCandidates.size());
            reflection.writeState(outputPath, false, scanning ? "discovering" : "collecting");
        };
        const auto logErrors = [&]() {
            std::size_t n = 0;
            for (const auto& e : reflection.errors) if (n++ < 20) ebon::console::log("warn", e);
            for (const auto& e : reflection.unresolved) if (n++ < 20) ebon::console::log("warn", e);
            if (n > 20) ebon::console::log("warn", "remaining errors are recorded in dump_state.json");
        };
        try {
            unsigned attempt = 0;
            std::int32_t lastObjectCount=-1;
            ULONGLONG stableSince=0;
            while (GetTickCount64() < deadline) {
                ebon::console::logf("info", "discovery attempt %u; deadline in %llu seconds", ++attempt,
                                    (unsigned long long)((deadline - GetTickCount64()) / 1000));
                snapshot.reset();
                if (reflection.discover()) {
                    const auto count=reflection.objectCount();
                    if(count!=lastObjectCount){lastObjectCount=count;stableSince=GetTickCount64();}
                    if(GetTickCount64()-stableSince<(ULONGLONG)settleSeconds*1000){
                        reflection.progressPhase="engine_initializing";
                        ebon::console::logf("info","engine is still registering objects (%d); waiting for a stable sample",count);
                    }else{
                        ebon::console::logf("success", "reflection discovered: name_pool=%s object_array=%s; collecting",
                                            ebon::hex(reflection.poolAddress() - mod.base).c_str(),
                                            ebon::hex(reflection.objectArrayAddress() - mod.base).c_str());
                        snapshot.reset();
                        const bool ok = reflection.collect();
                        ebon::console::logf("info","validated field layout: next=0x%llx name=0x%llx array=0x%llx offset=0x%llx",
                                            (unsigned long long)reflection.layout().fieldNext,(unsigned long long)reflection.layout().fieldName,
                                            (unsigned long long)reflection.layout().propArray,(unsigned long long)reflection.layout().propOffset);
                        if(ok){
                            reflection.report("writing",reflection.objects.size(),reflection.objects.size());
                            reflection.write(outputPath,true);
                            updateVersion(outputPath,true);
                            ebon::console::logf("success","dump finished: objects=%zu types=%zu properties=%zu success=1 output=%s",
                                                reflection.objects.size(),reflection.types.size(),reflection.propertyCount,outputPath.u8string().c_str());
                            const auto watchSeconds=ini.number("network_watch_seconds",0);
                            const bool resourceHotkey=config.number("resource_hotkey",1)!=0;
                            const auto versionWatchSeconds=config.number("network_version_watch_seconds",900);
                            if(versionWatchSeconds<0||versionWatchSeconds>3600)throw std::runtime_error("network_version_watch_seconds must be in 0..3600");
                            const bool versionWatch=config.number("network_version_collect",1)&&
                                config.number("network_version_getter_rva",0)>0&&versionWatchSeconds>0;
                            if(watchSeconds<0||watchSeconds>1800)throw std::runtime_error("network_watch_seconds must be in 0..1800");
                            if(resourceHotkey||watchSeconds>0||versionWatch){
                                if(watchSeconds>0)ebon::console::log("info","network recapture watcher ready; create network_capture.request in the capture directory to take a new snapshot");
                                if(resourceHotkey)ebon::console::log("success","F10 resource dump ready in the game or its log window: tables, assets, loaded maps/actors/components, script assets and references. Repeat in each scene or feature screen. Offline files: use dump-res. Output: resources/res_N");
                                const auto until=GetTickCount64()+(ULONGLONG)watchSeconds*1000;
                                const auto versionUntil=GetTickCount64()+(ULONGLONG)versionWatchSeconds*1000;
                                ULONGLONG nextVersionCheck=0;
                                unsigned serial=0,resourceSerial=0;
                                bool wasF10=(GetAsyncKeyState(VK_F10)&0x8000)!=0;
                                while(resourceHotkey||GetTickCount64()<until||(versionWatch&&GetTickCount64()<versionUntil)){
                                    if(versionWatch&&GetTickCount64()<versionUntil&&GetTickCount64()>=nextVersionCheck){
                                        nextVersionCheck=GetTickCount64()+1000;
                                        try{updateVersion(outputPath,false);}
                                        catch(const std::exception& e){ebon::console::logf("warn","NetworkVersion observation failed: %s",e.what());}
                                    }
                                    DWORD foregroundPid=0;
                                    GetWindowThreadProcessId(GetForegroundWindow(),&foregroundPid);
                                    const SHORT f10State=GetAsyncKeyState(VK_F10);
                                    const bool pressedEdge=ebon::keyPressedEdge(f10State,wasF10);
                                    const auto resourceRequest=outputPath/"resource_capture.request";
                                    const bool requested=resourceHotkey&&GetFileAttributesW(resourceRequest.c_str())!=INVALID_FILE_ATTRIBUTES;
                                    const bool validForeground=ebon::isForegroundForGame(foregroundPid,GetCurrentProcessId(),logViewerPid);
                                    const bool pressed=resourceHotkey&&pressedEdge&&validForeground;
                                    if(resourceHotkey&&pressedEdge&&!validForeground)
                                        ebon::console::logf("warn","F10 press observed outside game/log window; foreground pid=%lu game pid=%lu log pid=%lu",
                                                            foregroundPid,GetCurrentProcessId(),logViewerPid);
                                    if(pressed||requested){
                                        if(requested){
                                            DeleteFileW(resourceRequest.c_str());
                                            ebon::console::log("info","resource_capture.request detected");
                                        }else ebon::console::log("info","F10 key press detected in foreground game/log window");
                                        std::filesystem::path resourceOutput;
                                        do {resourceOutput=outputPath/"resources"/("res_"+std::to_string(++resourceSerial));}
                                        while(std::filesystem::exists(resourceOutput));
                                        captureResources(config,mod,digest,resourceOutput);
                                        // A held key triggers once, including during a long capture.
                                        wasF10=(GetAsyncKeyState(VK_F10)&0x8000)!=0;
                                    }
                                    const auto request=outputPath/"network_capture.request";
                                    if(GetTickCount64()<until&&GetFileAttributesW(request.c_str())!=INVALID_FILE_ATTRIBUTES){
                                        DeleteFileW(request.c_str());
                                        const auto followup=outputPath/("network_snapshot_"+std::to_string(++serial));
                                        try{
                                            ebon::Config nextConfig;nextConfig.load(profilePath);
                                            if(nextConfig.text("exe_sha256")!=config.text("exe_sha256"))throw std::runtime_error("recapture profile changed target module hash");
                                            snapshot.reset();ebon::Reflection next(snapshot,nextConfig,mod);next.moduleSha256=digest;
                                            next.setDeadline(GetTickCount64()+180000);
                                            try{const bool ready=next.collect();next.write(followup,ready);
                                                ebon::saveNetworkVersion(followup,ebon::observeNetworkVersion(memory,nextConfig,mod,digest));
                                                updateVersion(outputPath,true);
                                                ebon::console::logf(ready?"success":"warn","network recapture finished: %s success=%d",followup.u8string().c_str(),ready?1:0);}
                                            catch(const std::exception& e){next.errors.push_back(e.what());next.writeState(followup,false,"failed");throw;}
                                        }catch(const std::exception& e){ebon::console::logf("error","network recapture failed: %s",e.what());}
                                    }
                                    Sleep(10);
                                }
                            }
                            return 0;
                        }
                        ebon::console::logf("warn","metadata is not ready or layout validation failed: objects=%zu types=%zu properties=%zu; retrying",
                                            reflection.objects.size(),reflection.types.size(),reflection.propertyCount);
                        for(std::size_t sample=0;sample<std::min<std::size_t>(5,reflection.invalidObjectSamples.size());++sample)
                            ebon::console::log("debug",reflection.invalidObjectSamples[sample]);
                        reflection.write(outputPath,false);
                    }
                }
                logErrors();
                reflection.writeState(outputPath, false, "waiting");
                const auto now = GetTickCount64();
                if (now >= deadline) break;
                Sleep((DWORD)std::min<ULONGLONG>(1000, deadline - now));
            }
            reflection.errors.push_back("timed out waiting for validated engine reflection; check layout and candidate diagnostics");
        } catch (const std::exception& e) {
            reflection.errors.push_back(e.what());
        }
        logErrors();
        if(!reflection.objects.empty())reflection.write(outputPath,false);
        else reflection.writeState(outputPath,false,"failed");
        ebon::console::log("error", "capture failed; diagnostics saved to dump_state.json");
        return 2;
    } catch (const std::exception& e) {
        ebon::console::logf("error", "capture failed: %s", e.what());
        if (outputReserved) {
            try {
                ebon::save(outputPath / "dump_state.json",
                           "{\"format\":\"EbonReflection-v1\",\"status\":\"failed\",\"success\":false,\"errors\":[" +
                           ebon::quote(e.what()) + "]}\n");
            } catch (...) {}
        }
        return 1;
    } catch (...) {
        ebon::console::log("error", "unexpected worker exception");
        return 1;
    }
}

extern "C" __declspec(dllexport) int EbonDumpW(const wchar_t* configPath,const wchar_t* outputPath){if(!configPath||!outputPath)return 2;try{validateOutput(configPath,outputPath);}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 2;}return dump(GetCurrentProcessId(),configPath,outputPath);}

BOOL WINAPI DllMain(HINSTANCE instance,DWORD reason,LPVOID){
    if(reason==DLL_PROCESS_ATTACH){
        selfModule=instance;
        DisableThreadLibraryCalls(instance);
        HANDLE th=CreateThread(nullptr,0,injectedWorker,instance,0,nullptr);
        if(th)CloseHandle(th);
        else OutputDebugStringA("[ebon_dumper] failed to start injected worker thread\n");
    }
    return TRUE;
}
#else
int wmain(int argc,wchar_t** argv) {
    bool allowNonElevated = false;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--help" || a == L"-h") {
            std::cout << "EbonDumper --pid <PID> --profile <profile.ini> --out <new-output-directory> [--no-elevate] [--network-version-only]\n"
                      << "Read-only process reflection capture; auto-elevates with UAC runas if not administrator.\n";
            return 0;
        }
        if (a == L"--no-elevate") {
            allowNonElevated = true;
        }
    }
    if (!ebon::isElevated() && !allowNonElevated) {
        std::cout << "[ebon_dumper] not elevated: auto-elevating via UAC runas...\n";
        if (ebon::relaunchSelfElevated()) return 0;
        std::cerr << "[ebon_dumper] ERROR: UAC elevation required and was refused.\n";
        return 5;
    }
    ebon::enableDebugPrivilege();

    DWORD pid=0;std::filesystem::path profile,out;bool versionOnly=false;
    try {for(int i=1;i<argc;++i){std::wstring arg=argv[i];if(arg==L"--no-elevate")continue;if(arg==L"--network-version-only"){versionOnly=true;continue;}if(i+1>=argc)throw std::runtime_error("missing argument value");std::wstring value=argv[++i];if(arg==L"--pid"){std::size_t used=0;auto p=std::stoull(value,&used,0);if(used!=value.size()||!p||p>MAXDWORD)throw std::runtime_error("invalid PID");pid=(DWORD)p;}else if(arg==L"--profile")profile=value;else if(arg==L"--out")out=value;else throw std::runtime_error("unknown argument: "+ebon::utf8(arg));}
        if(!pid||profile.empty()||out.empty())throw std::runtime_error("required: --pid <PID> --profile <profile.ini> --out <directory>");
        validateOutput(profile,out);return versionOnly?dumpNetworkVersion(pid,profile,out):dump(pid,profile,out);
    }catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 2;}
}
#endif
