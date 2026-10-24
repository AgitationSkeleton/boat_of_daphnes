@echo off
rem Sets up Visual Studio's x64 environment for the build scripts (call it, don't run it): finds the newest Visual
rem Studio (2022 or newer, any edition or the Build Tools) that has the Clang component, with vswhere, drops
rem devkitPro/MSYS2/MinGW from PATH (their pkg-config and zlib must not be picked up), and sets
rem   VSROOT  the installation folder        CMAKE  VS's own cmake.exe       (clang-cl, Ninja and CMake are on PATH)
set "VSROOT="
set "VSDIR=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
if not exist "%VSDIR%\vswhere.exe" goto :fallback
pushd "%VSDIR%"
for /f "usebackq delims=" %%i in (`.\vswhere.exe -latest -products * -version [17.0^,^) -requires Microsoft.VisualStudio.Component.VC.Llvm.Clang -property installationPath`) do if not defined VSROOT set "VSROOT=%%i"
popd
if defined VSROOT goto :found
:fallback
for %%d in ("%ProgramFiles%\Microsoft Visual Studio\18" "%ProgramFiles%\Microsoft Visual Studio\2022") do for %%e in (Community Professional Enterprise BuildTools) do if not defined VSROOT if exist "%%~d\%%e\VC\Auxiliary\Build\vcvars64.bat" set "VSROOT=%%~d\%%e"
if defined VSROOT goto :found
echo Visual Studio 2022 or newer with the "C++ Clang tools for Windows" component not found.>&2
exit /b 1
:found
set "PKG_CONFIG="
set "PKG_CONFIG_PATH="
for /f "usebackq delims=" %%p in (`powershell -NoProfile -Command "($env:PATH -split ';' | Where-Object { $_ -and $_ -notmatch 'devkitpro|msys|mingw' }) -join ';'"`) do set "PATH=%%p"
set "PATH=%PATH%;%VSDIR%"
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "CMAKE=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "PATH=%VSROOT%\VC\Tools\Llvm\x64\bin;%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
exit /b 0
