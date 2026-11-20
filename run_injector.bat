@echo off
chcp 65001 >nul
cd /d "%~dp0"
echo EbonCore AES capture: start the game from its launcher when prompted.
echo After injection, press F9 inside the EbonCore Dumper terminal to hunt the AES key.
python tools\ebon_cli.py inject-and-dump --injector build-aes\EbonDebugInject.exe --dumper build-aes\EbonDumper.dll --wait-aes --no-sdk %*
set "EBON_CAPTURE_EXIT=%ERRORLEVEL%"
echo Capture exit code: %EBON_CAPTURE_EXIT%. See this capture's aes\aes_state.json (or aes_N for repeat presses).
exit /b %EBON_CAPTURE_EXIT%
