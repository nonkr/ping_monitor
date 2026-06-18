@echo off
setlocal EnableExtensions EnableDelayedExpansion

pushd "%~dp0"

set "VSWHERE=C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"

where cl.exe >nul 2>nul
if errorlevel 1 (
    if not exist "!VSWHERE!" (
        echo MSVC compiler was not found. Please install Visual Studio Build Tools with Desktop development with C++.
        popd
        exit /b 1
    )

    for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
        set "VSINSTALL=%%i"
    )

    if not defined VSINSTALL (
        echo Visual Studio C++ tools were not found.
        popd
        exit /b 1
    )

    call "!VSINSTALL!\VC\Auxiliary\Build\vcvars64.bat"
)

if not exist build mkdir build

rc.exe /nologo /fo build\app.res src\app.rc
if errorlevel 1 (
    popd
    exit /b 1
)

cl.exe /nologo /std:c++17 /EHsc /O2 /MT /utf-8 /DUNICODE /D_UNICODE ^
    /Fo:build\ /Fe:build\PingMonitor.exe src\main.cpp build\app.res ^
    user32.lib gdi32.lib shell32.lib iphlpapi.lib ws2_32.lib comctl32.lib dwmapi.lib uxtheme.lib ^
    /link /SUBSYSTEM:WINDOWS

set "BUILD_RESULT=%ERRORLEVEL%"
if "%BUILD_RESULT%"=="0" (
    echo.
    echo Built: %CD%\build\PingMonitor.exe
)

popd
exit /b %BUILD_RESULT%
