@echo off
rem Builds a target (default: tww) in Visual Studio's environment (tools\vsenv.cmd: clang-cl, CMake and Ninja from
rem VS, devkitPro/MSYS2 off PATH), configuring first if needed - a new source file makes CMake reconfigure, which
rem needs the compiler found.
rem   tools\build.cmd [target]
setlocal
set TARGET=%1
if "%TARGET%"=="" set TARGET=tww
call "%~dp0vsenv.cmd" || exit /b 1
cd /d "%~dp0.."
if not exist build\windows-clang\CMakeFiles\rules.ninja "%CMAKE%" --preset windows-clang || exit /b 1
"%CMAKE%" --build build\windows-clang --target %TARGET%
