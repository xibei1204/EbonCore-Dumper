@echo off
setlocal
chcp 65001 >nul
pushd "%~dp0"
set "EBON_FRIEND_BUILD=build-friend-capture"
echo Close the running client first. Building the current Friend sampler source.
rem Always rebuild: an existing DLL may predate the sampler changes.
call build_native.bat "%EBON_FRIEND_BUILD%"
if errorlevel 1 goto :failed
echo Start the client from its launcher after the injector begins waiting.
echo Wait for Friend revision=friend-capture-v2, then log in and open the friend list.
echo Use arm_friend_capture.bat if the F8 hotkey does not reopen the window.
python tools\ebon_cli.py inject-and-dump --injector "%EBON_FRIEND_BUILD%\EbonDebugInject.exe" --dumper "%EBON_FRIEND_BUILD%\EbonDumper.dll" --profile profiles\nte-ue5-friend.ini --seconds 300 --no-sdk %*
set "EBON_FRIEND_EXIT=%ERRORLEVEL%"
popd
exit /b %EBON_FRIEND_EXIT%
:failed
set "EBON_FRIEND_EXIT=%ERRORLEVEL%"
popd
exit /b %EBON_FRIEND_EXIT%
