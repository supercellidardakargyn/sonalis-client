@echo off
setlocal
cd /d "%~dp0.."
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64
if errorlevel 1 exit /b %errorlevel%
set "SONALIS_CMAKE=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "SONALIS_CTEST=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"
"%SONALIS_CMAKE%" --preset canary-release
if errorlevel 1 exit /b %errorlevel%
"%SONALIS_CMAKE%" --build --preset canary-release
if errorlevel 1 exit /b %errorlevel%
"%SONALIS_CTEST%" --preset canary-release
exit /b %errorlevel%
