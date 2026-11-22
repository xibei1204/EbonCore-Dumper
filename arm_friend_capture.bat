@echo off
setlocal
chcp 65001 >nul
pushd "%~dp0"
python tools\friend_capture.py arm %*
set "EBON_FRIEND_EXIT=%ERRORLEVEL%"
popd
exit /b %EBON_FRIEND_EXIT%
