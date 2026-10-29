@echo off
rem Build with MinGW-w64 (w64devkit). Needs gcc and windres on PATH.
setlocal enabledelayedexpansion

where gcc >nul 2>&1 || (
  echo gcc not found on PATH.
  echo Open the w64devkit shell, or add its bin directory to PATH, then retry.
  exit /b 1
)

set OUT=build
if not exist %OUT% mkdir %OUT%

rem MinGW's gcc spec always links its own default-manifest.o and binutils
rem refuses to merge two non-default manifests, so ours is compiled to an
rem object of that exact name and -B makes it win the startfile search.
rem The prefix MUST be absolute and end in a separator, or gcc finds neither
rem and the exe ships with no manifest at all (and never elevates).
set "PFX=%~dp0%OUT%"
set "PFX=%PFX:\=/%/"

set CFLAGS=-std=c11 -O2 -municode -mwindows -DUNICODE -D_UNICODE ^
 -Wall -Wextra -Wshadow -Wvla -Wpointer-arith -Wstrict-prototypes ^
 -fstack-protector-strong -fno-common -fno-strict-aliasing

rem ASLR, high-entropy ASLR, DEP. MinGW has no Control Flow Guard;
rem build-msvc.bat adds /guard:cf and /CETCOMPAT if you want those.
rem -static pulls libssp/libgcc in rather than importing libssp-0.dll, which
rem -fstack-protector-strong would otherwise make a runtime dependency.
set LDFLAGS=-B%PFX% -static -Wl,--nxcompat -Wl,--dynamicbase -Wl,--high-entropy-va -s

set LIBS=-lole32 -loleaut32 -lwintrust -lcrypt32 -lbcrypt -ladvapi32 ^
 -lshell32 -lshlwapi -lcomctl32 -luser32 -lgdi32

echo [1/3] resources
windres res\app.rc -O coff -o %OUT%\app.res
if errorlevel 1 exit /b 1

echo [2/3] manifest
windres res\manifest.rc -O coff -o %OUT%\default-manifest.o
if errorlevel 1 exit /b 1

echo [3/3] compile and link
gcc %CFLAGS% ^
  src\main.c src\wizard.c src\uia.c src\nv.c src\sec.c src\log.c ^
  %OUT%\app.res -o %OUT%\NvDriverKit.exe %LDFLAGS% %LIBS%
if errorlevel 1 exit /b 1

echo.
echo Built %OUT%\NvDriverKit.exe
for %%F in (%OUT%\NvDriverKit.exe) do echo Size: %%~zF bytes
