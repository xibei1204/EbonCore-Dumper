@echo off
chcp 65001 >nul
cd /d "%~dp0"
echo EbonCore AES capture: start the game from its launcher when prompted.
python tools\ebon_cli.py inject-and-dump --injector build-aes\EbonDebugInject.exe --dumper build-aes\EbonDumper.dll --wait-aes --no-sdk %*
set "EBON_CAPTURE_EXIT=%ERRORLEVEL%"
echo Capture exit code: %EBON_CAPTURE_EXIT%. See this capture's aes\aes_state.json.
exit /b %EBON_CAPTURE_EXIT%
