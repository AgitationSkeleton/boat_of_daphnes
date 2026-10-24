# Dot-source to get Visual Studio's x64 developer environment (newest VS 2022 or newer with the Clang component, found
# with vswhere) plus VS's LLVM, CMake and Ninja on PATH; devkitPro/MSYS2/MinGW are dropped from PATH. Sets $vs.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = $null
if (Test-Path $vswhere) {
    $vs = (& $vswhere -latest -products * -version '[17.0,)' -requires Microsoft.VisualStudio.Component.VC.Llvm.Clang -property installationPath | Select-Object -First 1)
}
if (-not $vs) {
    foreach ($base in "$env:ProgramFiles\Microsoft Visual Studio\18", "$env:ProgramFiles\Microsoft Visual Studio\2022", "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022") {
        foreach ($ed in 'Community', 'Professional', 'Enterprise', 'BuildTools') {
            if (-not $vs -and (Test-Path "$base\$ed\VC\Auxiliary\Build\vcvars64.bat")) { $vs = "$base\$ed" }
        }
    }
}
if (-not $vs) { throw 'Visual Studio 2022 or newer with the "C++ Clang tools for Windows" component not found' }
$env:PKG_CONFIG = $null
$env:PKG_CONFIG_PATH = $null
$env:PATH = (($env:PATH -split ';') | Where-Object { $_ -and $_ -notmatch 'devkitpro|msys|mingw' }) -join ';'
$env:PATH = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer;" + $env:PATH
$envLines = & "$env:SystemRoot\System32\cmd.exe" /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul && set"
foreach ($l in $envLines) { if ($l -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] } }
$env:PATH = "$vs\VC\Tools\Llvm\x64\bin;$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;" + $env:PATH
