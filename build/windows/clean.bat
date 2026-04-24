@echo off
setlocal

set "MSBUILD=C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe"
set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Release"
set "PLATFORM=%~2"
if "%PLATFORM%"=="" set "PLATFORM=x64"

"%MSBUILD%" "%~dp0..\..\projects\raygen-win32\raygen.sln"                 /nologo /t:Clean /p:Configuration=%CONFIG% /p:Platform=%PLATFORM%
"%MSBUILD%" "%~dp0..\..\projects\raygen-viewer-win32\raygen-viewer.sln"   /nologo /t:Clean /p:Configuration=%CONFIG% /p:Platform=%PLATFORM%
endlocal
