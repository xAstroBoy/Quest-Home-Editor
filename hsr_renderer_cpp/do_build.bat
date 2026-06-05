@echo off
REM Self-contained build: do NOT rely on vcvars64.bat (its vswhere lookup is
REM broken on this box, so it leaves the Windows SDK out of LIB -> LNK1181
REM cannot open kernel32.lib). We detect the toolchain dirs and set env directly.

set "VSBT=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools"
set "VCDIR=%VSBT%\VC\Tools\MSVC"
set "WK=C:\Program Files (x86)\Windows Kits\10"

REM Pick latest MSVC toolset (last entry alphabetically = newest)
set "MSVCVER="
for /f "delims=" %%d in ('dir /b /ad "%VCDIR%"') do set "MSVCVER=%%d"

REM Pick latest Windows SDK version that actually has um\x64\kernel32.lib
set "SDKVER="
for /f "delims=" %%d in ('dir /b /ad "%WK%\Lib"') do if exist "%WK%\Lib\%%d\um\x64\kernel32.lib" set "SDKVER=%%d"

set "MSVCROOT=%VCDIR%\%MSVCVER%"
set "INCLUDE=%MSVCROOT%\include;%WK%\Include\%SDKVER%\ucrt;%WK%\Include\%SDKVER%\um;%WK%\Include\%SDKVER%\shared;%WK%\Include\%SDKVER%\winrt"
set "LIB=%MSVCROOT%\lib\x64;%WK%\Lib\%SDKVER%\ucrt\x64;%WK%\Lib\%SDKVER%\um\x64"
set "CMAKEDIR=%VSBT%\Common7\IDE\CommonExtensions\Microsoft\CMake"
set "PATH=%MSVCROOT%\bin\Hostx64\x64;%CMAKEDIR%\Ninja;%CMAKEDIR%\CMake\bin;%PATH%"

echo MSVCVER=%MSVCVER%
echo SDKVER=%SDKVER%
cd /d "D:\Quest Stuff\Restore Old Envs\hsr_renderer_cpp\build"
ninja
echo NINJA_EXIT=%ERRORLEVEL%
