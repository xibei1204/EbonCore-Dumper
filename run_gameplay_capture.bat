@echo off
setlocal
chcp 65001 >nul
pushd "%~dp0"
set "EBON_GAMEPLAY_BUILD=build-gameplay-character-capture"

if not exist "profiles\nte-ue5-gameplay.ini" (
    echo [!] Missing profile: profiles\nte-ue5-gameplay.ini
    popd
    exit /b 2
)

if not exist "%EBON_GAMEPLAY_BUILD%\EbonDebugInject.exe" goto :build
if not exist "%EBON_GAMEPLAY_BUILD%\EbonDumper.dll" goto :build
if not exist "%EBON_GAMEPLAY_BUILD%\EbonLogConsole.exe" goto :build
goto :run

:build
echo [!] Native tools are missing. Building them now...
call build_native.bat "%EBON_GAMEPLAY_BUILD%"
if errorlevel 1 goto :failed
if not exist "%EBON_GAMEPLAY_BUILD%\EbonDebugInject.exe" goto :failed
if not exist "%EBON_GAMEPLAY_BUILD%\EbonDumper.dll" goto :failed
if not exist "%EBON_GAMEPLAY_BUILD%\EbonLogConsole.exe" goto :failed

:run

echo EbonCore gameplay capture is starting.
echo Start HTGame.exe from its launcher when prompted.
echo Sampling starts automatically for 180 seconds after validation. Log in during this window.
echo Press F8 in the game or dumper log window to restart the sampling window.
echo The log window title keeps the current F8 state visible.
echo F10 captures loaded gameplay resources.
echo Player character roots now have a separate capture budget.
echo Open the target character preview before F10; a portrait alone may load only its mesh.
echo.
python tools\ebon_cli.py inject-and-dump --injector "%EBON_GAMEPLAY_BUILD%\EbonDebugInject.exe" --game HTGame.exe --dumper "%EBON_GAMEPLAY_BUILD%\EbonDumper.dll" --profile profiles\nte-ue5-gameplay.ini --no-sdk %*
set "EBON_CAPTURE_EXIT=%ERRORLEVEL%"
echo Capture exit code: %EBON_CAPTURE_EXIT%
popd
exit /b %EBON_CAPTURE_EXIT%

:failed
set "EBON_CAPTURE_EXIT=%ERRORLEVEL%"
if "%EBON_CAPTURE_EXIT%"=="0" set "EBON_CAPTURE_EXIT=1"
popd
exit /b %EBON_CAPTURE_EXIT%
