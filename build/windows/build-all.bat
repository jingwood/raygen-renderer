@echo off
setlocal

rem Build both raygen.exe and raygen-viewer.exe.
rem Usage: build-all.bat [Release|Debug] [x64]

set "SCRIPT_DIR=%~dp0"

call "%SCRIPT_DIR%build-raygen.bat" %*
if errorlevel 1 exit /b 1

call "%SCRIPT_DIR%build-viewer.bat" %*
if errorlevel 1 exit /b 1

echo.
echo All builds succeeded.
endlocal
