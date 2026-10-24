# Makes the player's release: dist\boat-of-daphnes-<version>-windows-x64.zip, everything the program needs beside it and
# nothing of the game. The player unzips it, starts boat-of-daphnes.exe and picks their disc; the first start builds the
# game and the randomizer from it (src/setup/firstrun.cpp), with the bundled compiler (tools\llvm) and the randomizer's
# generator (rando\: an embedded Python, wwrando and gclib, the seed and patch scripts).
#
#   powershell -File tools\package.ps1 [-Version 1.0.0] [-Build build\windows-clang] [-Llvm <clang folder>]  (default: buildlvm, else Visual Studio's own) [-NoZip]
#
# Checks before zipping: every file the first start needs is there, and nothing made from a disc is (game code, game\,
# the extras the first start makes: assets\link_colors, swift_sail, bunnyhood).
param(
    [string]$Version = "",
    [string]$Build = "build\windows-clang",
    [string]$Runtime = "build\windows\rando_runtime",
    [string]$Llvm = "",
    [switch]$NoZip
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
if (-not $Version) {
    $Version = (git describe --tags --always 2>$null)
    if (-not $Version) { $Version = (Get-Date -Format "yyyy.MM.dd") }
}
$name = "boat-of-daphnes-$Version-windows-x64"
$out = Join-Path "dist" $name
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force $out | Out-Null

function Need([string]$path) { if (-not (Test-Path $path)) { throw "missing: $path (build it first)" } }
function Put([string]$from, [string]$to) {
    Need $from
    $dest = Join-Path $out $to
    New-Item -ItemType Directory -Force (Split-Path -Parent $dest) | Out-Null
    Copy-Item -Recurse -Force $from $dest
}

# --- the program and what it loads ---
foreach ($f in 'boat-of-daphnes.exe', 'boat-of-daphnes.lib', 'SDL3.dll', 'webgpu_dawn.dll', 'dxcompiler.dll', 'dxil.dll',
               'libpng16.dll', 'zlib1.dll') {
    Put (Join-Path $Build $f) $f
}
# Visual Studio (2022 or newer, with the Clang component): the redistributable runtime and, if none is given, clang
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = $null
if (Test-Path $vswhere) {
    $vs = (& $vswhere -latest -products * -version '[17.0,)' -requires Microsoft.VisualStudio.Component.VC.Llvm.Clang -property installationPath | Select-Object -First 1)
}

# the Visual C++ runtime (the program is built with the MSVC STL): the newest VC14x CRT folder
$redist = $null
if ($vs) {
    $redist = Get-ChildItem "$vs\VC\Redist\MSVC\*\x64\Microsoft.VC14*.CRT" -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -notmatch '\\(onecore|debug_nonredist)\\' } |
        Sort-Object { [version]($_.FullName -replace '^.*\\MSVC\\([\d.]+)\\.*$', '$1') } | Select-Object -Last 1
}
if (-not $redist) { throw "no Visual C++ redistributable folder found (VC\Redist\MSVC\<version>\x64\Microsoft.VC14x.CRT of Visual Studio 2022 or newer with the Clang component)" }
foreach ($f in 'msvcp140.dll', 'msvcp140_atomic_wait.dll', 'vcruntime140.dll', 'vcruntime140_1.dll') {
    Put (Join-Path $redist.FullName $f) $f
}

# --- the compiler the first start builds the game's code with ---
if (-not $Llvm) {
    foreach ($c in "build\llvm", "..\SA2BRecomp\build\llvm") { if (Test-Path "$c\clang.exe") { $Llvm = $c; break } }
}
if (-not $Llvm -and $vs -and (Test-Path "$vs\VC\Tools\Llvm\x64\bin\clang.exe")) { $Llvm = "$vs\VC\Tools\Llvm\x64\bin" }
if (-not $Llvm) { throw "no clang folder (clang.exe, lld-link.exe): pass -Llvm" }
foreach ($f in 'clang.exe', 'lld-link.exe') { Put (Join-Path $Llvm $f) "tools\llvm\$f" }
# LLVM's licence (Apache-2.0 with LLVM exceptions) goes with its binaries; Visual Studio's folder has none
$llvmLicense = Join-Path $Llvm 'LICENSE.TXT'
if (-not (Test-Path $llvmLicense)) { $llvmLicense = Join-Path $PSScriptRoot 'llvm-LICENSE.TXT' }
Put $llvmLicense "tools\llvm\LICENSE.TXT"

# --- the randomizer's generator ---
Need "$Runtime\python\python.exe"
foreach ($d in 'python', 'site-packages', 'wwrando', 'gclib') { Put "$Runtime\$d" "rando\$d" }
Put "$Runtime\manifest.json" "rando\manifest.json"
foreach ($f in 'generate_seed.py', 'patch_game.py', 'describe_options.py', 'prepare_extras.py', 'variant_sites.py',
               'link_colors.py', 'swift_sail.py', 'swift_sail_data.json', 'bunny_hood.py', 'check_flags.json',
               'check_logic_server.py') {  # (the Check Tracker's logic helper: without it the tracker says the runtime is missing)
    Put "tools\rando\$f" "rando\$f"
}
Put "tools\rando\bunny" "rando\bunny"
if (Test-Path "$out\rando\bunny\blender") { Remove-Item -Recurse -Force "$out\rando\bunny\blender" }
Get-ChildItem -Recurse -Directory -Filter "__pycache__" "$out\rando" | Remove-Item -Recurse -Force
# wwrando's reference options (the module the first start builds is patched with them) and the menu's option list
$py = "$Runtime\python\python.exe"
$env:PYTHONPATH = "$Runtime\site-packages"
$env:RANDO_RUNTIME = (Resolve-Path $Runtime).Path
& $py -c "import json,sys; sys.path.insert(0, r'tools\rando'); import variant_sites; open(r'$out\rando\reference-options.json','w').write(json.dumps(variant_sites.REFERENCE_OPTIONS, indent=1) + '\n')"
New-Item -ItemType Directory -Force "$out\assets\randomizer" | Out-Null
& $py "tools\rando\describe_options.py" "$out\assets\randomizer\options.json" | Out-Null
Need "$out\assets\randomizer\options.json"

# --- assets, folders, papers ---
Put "assets\fonts" "assets\fonts"
New-Item -ItemType Directory -Force "$out\mods" | Out-Null
Set-Content "$out\mods\custom_mod_files_go_here.txt" "Mods, texture packs and player models go here (zips or folders). Settings > Mods lists them." -Encoding ascii
foreach ($f in 'README.md', 'LICENSE.md') { if (Test-Path $f) { Put $f $f } }
if (Test-Path "res\THIRD_PARTY_NOTICES.txt") { Put "res\THIRD_PARTY_NOTICES.txt" "THIRD_PARTY_NOTICES.txt" }
if (Test-Path "src\bod\LICENSE") { Put "src\bod\LICENSE" "licenses\boat-of-daphnes-LICENSE.txt" }
Put "assets\fonts\OFL.txt" "licenses\RocknRollOne-OFL.txt"

# --- checks ---
$forbidden = @('tww_game.dll', 'tww_game_rando.dll', 'tww_game.txt', 'tww_game_rando.txt', 'tww_game_rando.diff', 'game',
               'assets\link_colors', 'assets\swift_sail', 'assets\bunnyhood', 'Save', 'Randomizer', 'logs')
foreach ($f in $forbidden) { if (Test-Path (Join-Path $out $f)) { throw "the release must not carry $f (made from the player's disc)" } }
$discFiles = Get-ChildItem -Recurse $out -Include *.iso, *.gcm, *.ciso, *.rvz, *.wia, *.rel, *.dol, *.arc, *.bdl, *.bti -File |
    Where-Object { $_.FullName -notmatch '\\rando\\(wwrando|gclib|site-packages)\\' }
if ($discFiles) { throw "game files in the release: $($discFiles.FullName -join ', ')" }
foreach ($f in 'boat-of-daphnes.exe', 'boat-of-daphnes.lib', 'tools\llvm\clang.exe', 'tools\llvm\lld-link.exe',
               'rando\python\python.exe', 'rando\patch_game.py', 'rando\generate_seed.py', 'rando\bunny_hood.py',
               'rando\check_logic_server.py', 'rando\check_flags.json', 'assets\randomizer\options.json', 'assets\fonts\RocknRollOne-Regular.ttf') {
    Need (Join-Path $out $f)
}
$size = (Get-ChildItem -Recurse $out -File | Measure-Object Length -Sum).Sum / 1MB
Write-Host ("{0}: {1:N0} MB in {2} files" -f $out, $size, (Get-ChildItem -Recurse $out -File).Count)

if (-not $NoZip) {
    $zip = "dist\$name.zip"
    if (Test-Path $zip) { Remove-Item -Force $zip }
    # Windows PowerShell 5's Compress-Archive writes its entries with backslashes, which other unzippers (and Linux) take
    # as part of the file name: Windows' own tar (bsdtar, in System32 since Windows 10) writes a standard zip instead.
    $tar = Join-Path $env:SystemRoot "System32\tar.exe"
    if (Test-Path $tar) {
        & $tar -a -c -f $zip -C $out *
        if ($LASTEXITCODE) { throw "tar failed ($LASTEXITCODE)" }
    } else {
        Compress-Archive -Path "$out\*" -DestinationPath $zip -CompressionLevel Optimal
    }
    Write-Host ("{0}: {1:N0} MB" -f $zip, ((Get-Item $zip).Length / 1MB))
}
