@echo off
REM 2026-09-17: VS2022 Community (14.44) environment. The VS18 BuildTools env (14.51) mismatches the
REM cached 14.44 cl.exe (STL1001) and its STL breaks fmt 9.1.0 on a fresh configure - see toolchain notes.
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" amd64
set VCPKG_ROOT=C:\vcpkg
cd /d %~dp0
cmake --build build/release
if errorlevel 1 ( echo === BUILD FAILED === & exit /b 1 )
echo === BUILD SUCCEEDED ===
