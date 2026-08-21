@echo off
setlocal
cd /d "%~dp0"

rem Locate latest VS install via vswhere, fall back to fixed paths
set VCVARS=
for /f "usebackq delims=" %%i in (`"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do (
    if exist "%%i\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
)
if not defined VCVARS if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS (
    echo [ERROR] vcvars64.bat not found
    exit /b 1
)

call "%VCVARS%" >nul 2>&1
if errorlevel 1 (
    echo [ERROR] failed to load vcvars64
    exit /b 1
)

if not exist build mkdir build

echo [1/2] compiling resources...
rc /nologo /fo build\resource.res src\resource.rc
if errorlevel 1 (
    echo [ERROR] resource compile failed
    exit /b 1
)

echo [2/2] compiling sources and linking...
rem /WX: treat warnings as errors. /Fo /Fd: keep intermediates in build\ to keep root clean
cl /nologo /std:c++20 /EHsc /W4 /WX /utf-8 /O2 /MT /GL /Zi /DSECURITY_WIN32 /DUNICODE /D_UNICODE ^
   /Isrc /Fo:build\ /Fd:build\GotifyInbox.pdb ^
   src\main.cpp src\ui\main_window.cpp src\ui\tray.cpp ^
   src\ui\settings_dlg.cpp src\core\json.cpp src\core\config.cpp src\core\history.cpp src\util\utf8.cpp ^
   src\net\ws_client.cpp ^
   build\resource.res /Fe:build\GotifyInbox.exe ^
   /link /LTCG /OPT:REF /OPT:ICF /DEBUG /SUBSYSTEM:WINDOWS ^
   user32.lib kernel32.lib gdi32.lib shell32.lib comctl32.lib ws2_32.lib secur32.lib crypt32.lib

if errorlevel 1 (
    echo [ERROR] build failed
    exit /b 1
)

echo [OK] build\GotifyInbox.exe
