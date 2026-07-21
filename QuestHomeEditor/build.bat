@echo off
REM Compatibility entry point. The maintained Windows build logic lives in
REM scripts\build_windows.bat and discovers Visual Studio with vswhere.
call "%~dp0scripts\build_windows.bat" %*
exit /b %ERRORLEVEL%
