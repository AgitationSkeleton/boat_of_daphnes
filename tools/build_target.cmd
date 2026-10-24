@echo off
rem tools\build_target.cmd <target> [ninja args]: builds one target in Visual Studio's environment (tools\vsenv.cmd).
setlocal
call "%~dp0vsenv.cmd" || exit /b 1
cd /d "%~dp0.."
"%CMAKE%" --build build\windows-clang --target %* 
