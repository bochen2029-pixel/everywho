@echo off
rem everywho build - MSVC (VS2022), static CRT, /W4, zero warnings.
rem Produces everywho.exe (console: every mode) and everywho-gui.exe (windows subsystem: the
rem same binary without a console flash - double-click or pin it). Stage 0 adds the sources
rem listed in SRC; the tests build with build.bat tests.
setlocal enabledelayedexpansion
where cl >nul 2>nul
if errorlevel 1 (
  set "VCV=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
  if not exist "!VCV!" (
    set "VSWHERE=C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
    for /f "usebackq tokens=*" %%i in (`""!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath"`) do set "VSDIR=%%i"
    set "VCV=!VSDIR!\VC\Auxiliary\Build\vcvars64.bat"
  )
  if not exist "!VCV!" ( echo build: vcvars64.bat not found & exit /b 1 )
  call "!VCV!" >nul
)
set CXXFLAGS=/nologo /c /std:c++20 /O2 /W4 /WX /permissive- /EHsc /utf-8 /MT /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /Isrc
set LIBS=advapi32.lib tdh.lib user32.lib gdi32.lib gdiplus.lib shell32.lib ole32.lib uuid.lib
set SRC=src\everywho.cpp src\everywho_gui.cpp src\etw_session.cpp src\who.cpp src\where.cpp src\counters.cpp
set OBJ=everywho.obj everywho_gui.obj etw_session.obj who.obj where.obj counters.obj

if "%1"=="tests" goto tests

cl %CXXFLAGS% %SRC% || exit /b 1
rc /nologo /fo everywho.res everywho.rc || exit /b 1
link /nologo /SUBSYSTEM:CONSOLE /OUT:everywho.exe %OBJ% everywho.res %LIBS% || exit /b 1
link /nologo /SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup /OUT:everywho-gui.exe %OBJ% everywho.res %LIBS% || exit /b 1
echo OK: everywho.exe everywho-gui.exe
exit /b 0

:tests
cl %CXXFLAGS% tests\plant.cpp tests\harness.cpp tests\drive.cpp || exit /b 1
link /nologo /SUBSYSTEM:CONSOLE /OUT:everywho-plant.exe plant.obj || exit /b 1
link /nologo /SUBSYSTEM:CONSOLE /OUT:everywho-harness.exe harness.obj || exit /b 1
link /nologo /SUBSYSTEM:CONSOLE /OUT:everywho-drive.exe drive.obj user32.lib gdi32.lib gdiplus.lib ole32.lib || exit /b 1
echo OK: everywho-plant.exe everywho-harness.exe everywho-drive.exe
exit /b 0
