@echo off
rem Build with MSVC. Run from a "x64 Native Tools Command Prompt for VS".
rem Adds Control Flow Guard, CET shadow stack and /analyze, which MinGW
rem cannot provide.
setlocal

where cl >nul 2>&1 || (
  echo cl.exe not found.
  echo Open "x64 Native Tools Command Prompt for VS 2022" and retry.
  exit /b 1
)

set OUT=build
if not exist %OUT% mkdir %OUT%

set CFLAGS=/nologo /O2 /W4 /permissive- /sdl /GS /guard:cf /utf-8 ^
 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /Fo%OUT%\

rem MSVC has no default-manifest collision; it embeds ours directly.
set LDFLAGS=/link /SUBSYSTEM:WINDOWS /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA ^
 /GUARD:CF /CETCOMPAT /OPT:REF /OPT:ICF ^
 /MANIFEST:EMBED /MANIFESTINPUT:res\app.manifest

set LIBS=ole32.lib oleaut32.lib wintrust.lib crypt32.lib bcrypt.lib ^
 advapi32.lib shell32.lib shlwapi.lib comctl32.lib user32.lib gdi32.lib

echo [1/2] resources
rc /nologo /fo %OUT%\app.res res\app.rc
if errorlevel 1 exit /b 1

echo [2/2] compile and link
cl %CFLAGS% src\main.c src\wizard.c src\uia.c src\nv.c src\sec.c src\log.c ^
   %OUT%\app.res /Fe%OUT%\NvDriverKit.exe %LDFLAGS% %LIBS%
if errorlevel 1 exit /b 1

echo.
echo Built %OUT%\NvDriverKit.exe
