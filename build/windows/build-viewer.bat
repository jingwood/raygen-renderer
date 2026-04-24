@echo off
setlocal

set "MSBUILD=C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe"
set "SLN=%~dp0..\..\projects\raygen-viewer-win32\raygen-viewer.sln"

set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Release"
set "PLATFORM=%~2"
if "%PLATFORM%"=="" set "PLATFORM=x64"

"%MSBUILD%" "%SLN%" /nologo /m /p:Configuration=%CONFIG% /p:Platform=%PLATFORM%
if errorlevel 1 exit /b 1

echo.
echo raygen-viewer build succeeded: %~dp0..\..\projects\raygen-viewer-win32\x64\%CONFIG%\raygen-viewer.exe
endlocal
