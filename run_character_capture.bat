@echo off
setlocal
chcp 65001 >nul
pushd "%~dp0"
if not exist "build-character-loader\EbonDumper.dll" (
    call build_native.bat build-character-loader
    if errorlevel 1 goto :failed
)
echo Character loading uses the new opt-in DLL. Previously injected DLLs do not update automatically.
echo Start the local client when prompted; enter its game scene, then press Enter here.
python tools\capture_character.py %*
set "EBON_CHARACTER_EXIT=%ERRORLEVEL%"
echo Character capture exit code: %EBON_CHARACTER_EXIT%
popd
exit /b %EBON_CHARACTER_EXIT%
:failed
popd
exit /b 2
