@echo off
echo ================================================
echo  HSR Renderer — Build Script
echo  libshell.so Vulkan pipeline replica
echo ================================================
echo.

set "SDIR=%~dp0"
set "BDIR=%SDIR%build"

REM Set up MSVC x64 environment
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if %ERRORLEVEL% neq 0 (
    echo ERROR: vcvars64.bat failed
    exit /b 1
)
echo MSVC environment ready.
echo.

REM Add cmake and ninja to PATH
set "VSROOT=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake"
set "PATH=%VSROOT%\CMake\bin;%VSROOT%\Ninja;%PATH%"

if not exist "%BDIR%" mkdir "%BDIR%"

echo cmake location:
where cmake
echo.

echo Configuring with CMake ^(Ninja^)...
cmake -S "%SDIR%." -B "%BDIR%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DHSR_HAVE_PHYSX=ON
if %ERRORLEVEL% neq 0 (
    echo CMake configure FAILED
    exit /b 1
)

echo.
echo Building...
cmake --build "%BDIR%" --config Release
if %ERRORLEVEL% neq 0 (
    echo Build FAILED
    exit /b 1
)

echo.
echo ================================================
echo  Build SUCCESS
echo  Binary: %BDIR%\hsr_renderer.exe
echo.
echo  Usage: %BDIR%\hsr_renderer.exe ^<apk_path^>
echo ================================================
