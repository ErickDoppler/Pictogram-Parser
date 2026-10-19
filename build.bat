@echo off
rem Builds Pictogram Parser with the toolchain in C:\workenv
setlocal
set WORKENV=C:\workenv
set PATH=%WORKENV%\w64devkit\bin;%WORKENV%\cmake-4.4.3-windows-x86_64\bin;%WORKENV%\ninja;%PATH%

cd /d "%~dp0"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++ -DCMAKE_RC_COMPILER=windres
if errorlevel 1 goto :fail
cmake --build build
if errorlevel 1 goto :fail

echo.
echo Built: %~dp0build\PictogramParser.exe
exit /b 0

:fail
echo.
echo BUILD FAILED
exit /b 1
