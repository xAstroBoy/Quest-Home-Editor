@echo off
REM hsr_renderer / editor - one-command Windows build.
REM Auto-locates MSVC (any VS edition, via vswhere), configures with Ninja, builds Release.
REM Dependencies (GLFW, astcenc) are auto-fetched by CMake FetchContent; Vulkan headers +
REM everything else are vendored in third_party/ - no SDKs to install.
REM PhysX solid-collision cooking uses vendored Windows-only libs -> ON here (build_linux/macos turn it OFF).

setlocal enabledelayedexpansion
set "SDIR=%~dp0.."
set "BDIR=%SDIR%\build"

echo ================================================
echo  hsr_renderer - Windows build
echo ================================================

REM Locate MSVC via vswhere (works for any VS Community/Pro/BuildTools with the C++ toolset)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo ERROR: vswhere.exe not found. Install Visual Studio or Build Tools with the
    echo "Desktop development with C++" workload: https://visualstudio.microsoft.com/downloads/
    exit /b 1
)
set "VSPATH="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if "%VSPATH%"=="" (
    echo ERROR: no Visual Studio with the C++ toolset found. Add the
    echo "Desktop development with C++" workload in the VS Installer.
    exit /b 1
)
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if %ERRORLEVEL% neq 0 ( echo ERROR: vcvars64.bat failed & exit /b 1 )
echo MSVC: %VSPATH%

REM cmake + ninja: PATH first, else the copies VS ships
where cmake >nul 2>nul
if %ERRORLEVEL% neq 0 set "PATH=%VSPATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%PATH%"
where ninja >nul 2>nul
if %ERRORLEVEL% neq 0 set "PATH=%VSPATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
where cmake >nul 2>nul
if %ERRORLEVEL% neq 0 ( echo ERROR: cmake not found - install it or the VS "C++ CMake tools" component & exit /b 1 )

echo Configuring...
cmake -S "%SDIR%" -B "%BDIR%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DHSR_HAVE_PHYSX=ON
if %ERRORLEVEL% neq 0 ( echo ERROR: configure failed & exit /b 1 )

echo Building...
cmake --build "%BDIR%"
if %ERRORLEVEL% neq 0 ( echo ERROR: build failed & exit /b 1 )

echo.
echo ================================================
echo  Build SUCCESS
echo  Editor:  %BDIR%\hsr_renderer.exe
echo  Cooker:  %BDIR%\hsl_cook.exe
echo  Run:     hsr_renderer.exe ^<env.apk or .gltf.ovrscene^>
echo ================================================
exit /b 0
