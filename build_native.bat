@echo off
rem Build every native EbonCore target with the MSVC x64 toolchain.
rem Usage: build_native.bat [output-dir]        (default output dir: build)
setlocal
pushd "%~dp0"

set "EBON_BUILD_DIR=%~1"
if "%EBON_BUILD_DIR%"=="" set "EBON_BUILD_DIR=build"

rem Respect an explicitly selected toolchain; otherwise reuse an initialized
rem x64 environment or discover Visual Studio/Build Tools through vswhere.
if defined EBON_VCVARS (
    if not exist "%EBON_VCVARS%" (
        echo [x] EBON_VCVARS does not point to an existing vcvars64.bat.
        popd
        exit /b 1
    )
    call "%EBON_VCVARS%" >nul
) else (
    call :discover_msvc
)
where cl >nul 2>nul
if errorlevel 1 (
    echo [x] MSVC x64 toolchain not found. Run this script from an "x64 Native Tools" prompt.
    popd
    exit /b 1
)
if defined VSCMD_ARG_TGT_ARCH if /i not "%VSCMD_ARG_TGT_ARCH%"=="x64" (
    echo [x] This project requires the MSVC x64 target toolchain.
    popd
    exit /b 1
)

if not exist "%EBON_BUILD_DIR%" mkdir "%EBON_BUILD_DIR%"
if not exist "%EBON_BUILD_DIR%\obj" mkdir "%EBON_BUILD_DIR%\obj"

set "EBON_CFLAGS=/nologo /O2 /std:c++17 /EHsc /W4 /utf-8 /DUNICODE /D_UNICODE /I native"
set "EBON_OBJ=/Fo:%EBON_BUILD_DIR%\obj\"
set "EBON_OUT=%EBON_BUILD_DIR%"

echo [1/10] EbonLogConsole.exe
cl %EBON_CFLAGS% %EBON_OBJ%EbonLogConsole.obj native\dumper\EbonLogConsole.cpp /Fe:%EBON_OUT%\EbonLogConsole.exe
if errorlevel 1 goto :failed

echo [2/10] EbonDumper.exe
cl %EBON_CFLAGS% %EBON_OBJ%EbonDumper.obj native\dumper\EbonDumper.cpp /Fe:%EBON_OUT%\EbonDumper.exe bcrypt.lib advapi32.lib shell32.lib psapi.lib
if errorlevel 1 goto :failed

echo [3/10] EbonDumper.dll
cl %EBON_CFLAGS% /DEBON_DLL /LD %EBON_OBJ%EbonDumperDll.obj native\dumper\EbonDumper.cpp /Fe:%EBON_OUT%\EbonDumper.dll bcrypt.lib advapi32.lib shell32.lib psapi.lib
if errorlevel 1 goto :failed

echo [4/10] EbonReflectionSelfTest.exe
cl %EBON_CFLAGS% %EBON_OBJ%EbonReflectionSelfTest.obj native\sdk\EbonReflectionSelfTest.cpp /Fe:%EBON_OUT%\EbonReflectionSelfTest.exe
if errorlevel 1 goto :failed

echo [5/10] EbonDllSelfTest.exe
cl %EBON_CFLAGS% %EBON_OBJ%EbonDllSelfTest.obj native\sdk\EbonDllSelfTest.cpp /Fe:%EBON_OUT%\EbonDllSelfTest.exe
if errorlevel 1 goto :failed

echo [6/10] EbonDebugInject.exe
cl %EBON_CFLAGS% %EBON_OBJ%EbonDebugInject.obj native\injector\EbonDebugInject.cpp /Fe:%EBON_OUT%\EbonDebugInject.exe shell32.lib psapi.lib advapi32.lib
if errorlevel 1 goto :failed

echo [7/10] EbonAesFinder.exe
cl %EBON_CFLAGS% %EBON_OBJ%EbonAesFinder.obj native\aes\aes_finder.cpp /Fe:%EBON_OUT%\EbonAesFinder.exe
if errorlevel 1 goto :failed

echo [8/10] EbonAesScan.exe
cl %EBON_CFLAGS% %EBON_OBJ%EbonAesScan.obj native\aes\aes_mem_scanner.cpp /Fe:%EBON_OUT%\EbonAesScan.exe
if errorlevel 1 goto :failed

echo [9/10] EbonAesSelfTest.exe
cl %EBON_CFLAGS% %EBON_OBJ%EbonAesSelfTest.obj native\aes\test_aes.cpp /Fe:%EBON_OUT%\EbonAesSelfTest.exe
if errorlevel 1 goto :failed

echo [10/10] EbonResourceSelfTest.exe
cl %EBON_CFLAGS% %EBON_OBJ%EbonResourceSelfTest.obj native\sdk\EbonResourceSelfTest.cpp /Fe:%EBON_OUT%\EbonResourceSelfTest.exe
if errorlevel 1 goto :failed

echo [ok] targets written to %EBON_BUILD_DIR%
popd
exit /b 0

:failed
echo [x] build failed
popd
exit /b 1

:discover_msvc
set "EBON_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%EBON_VSWHERE%" exit /b 1
for /f "usebackq delims=" %%I in (`"%EBON_VSWHERE%" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find VC\Auxiliary\Build\vcvars64.bat`) do call "%%I" >nul
exit /b
