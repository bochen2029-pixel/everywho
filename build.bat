@echo off
rem everywho build - MSVC (VS2022), static CRT, /W4 /WX (zero warnings).
rem everywho.exe = console, every mode. everywho-gui.exe is linked when src\everywho_gui.cpp
rem exists (Stage 3). "build.bat tests" builds the oracle and the driver (Stage 1+).
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
set LIBS=advapi32.lib pdh.lib user32.lib gdi32.lib shell32.lib ole32.lib uuid.lib
set SRC=src\everywho.cpp src\who.cpp src\where.cpp src\counters.cpp src\shell.cpp
set OBJ=everywho.obj who.obj where.obj counters.obj shell.obj

if "%1"=="tests" goto tests

cl %CXXFLAGS% %SRC% || exit /b 1
rc /nologo /fo everywho.res everywho.rc || exit /b 1
link /nologo /SUBSYSTEM:CONSOLE /OUT:everywho.exe %OBJ% everywho.res %LIBS% || exit /b 1
if exist src\everywho_gui.cpp (
  cl %CXXFLAGS% src\everywho_gui.cpp || exit /b 1
  link /nologo /SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup /OUT:everywho-gui.exe %OBJ% everywho_gui.obj everywho.res %LIBS% gdiplus.lib || exit /b 1
  echo OK: everywho.exe everywho-gui.exe
) else (
  echo OK: everywho.exe
)
exit /b 0

:tests
cl %CXXFLAGS% tests\plant.cpp tests\harness.cpp tests\drive.cpp || exit /b 1
link /nologo /SUBSYSTEM:CONSOLE /OUT:everywho-plant.exe plant.obj || exit /b 1
link /nologo /SUBSYSTEM:CONSOLE /OUT:everywho-harness.exe harness.obj || exit /b 1
link /nologo /SUBSYSTEM:CONSOLE /OUT:everywho-drive.exe drive.obj user32.lib gdi32.lib gdiplus.lib ole32.lib || exit /b 1
echo OK: everywho-plant.exe everywho-harness.exe everywho-drive.exe
exit /b 0
