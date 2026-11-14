@echo off
pushd "%~dp0"
if not exist "build\EbonDebugInject.exe" (
    echo [!] build\EbonDebugInject.exe not found. Building now...
    call build_native.bat
    if errorlevel 1 exit /b 1
)
python tools\ebon_cli.py inject-and-dump --game HTGame.exe --dumper build\EbonDumper.dll --profile profiles\nte-ue5.ini %*
set "EBON_RUN_RESULT=%errorlevel%"
popd
exit /b %EBON_RUN_RESULT%
