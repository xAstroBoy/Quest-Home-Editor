@echo off
REM Used by build.py. Keep this wrapper path-independent so a clone can be
REM built from any drive or directory.
call "%~dp0scripts\build_windows.bat" %*
exit /b %ERRORLEVEL%
