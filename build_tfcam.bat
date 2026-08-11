@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" amd64
set PATH=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%
cd /d %~dp0
cmake --build build/release
if errorlevel 1 ( echo === BUILD FAILED === & exit /b 1 )
echo === BUILD SUCCEEDED ===
