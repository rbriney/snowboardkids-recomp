# Clean Windows release build (separate build folder) and a zip laid out like upstream's Windows release.
# Usage: powershell -ExecutionPolicy Bypass -File tools\build-windows-release.ps1 -Version 0.1.2
param([Parameter(Mandatory = $true)][string]$Version)
$ErrorActionPreference = "Stop"

$repo  = Split-Path $PSScriptRoot -Parent
$build = "$repo\build-windows-release"
$sdkCmake = "D:\Android\Sdk\cmake\3.31.6\bin"
$mipsLlvm = "D:\dev\tools\LLVM-19.1.3-Windows-X64\bin"

$env:Path = [Environment]::GetEnvironmentVariable("Path","Machine") + ";" + [Environment]::GetEnvironmentVariable("Path","User")
& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
$env:Path = ($env:Path -split ';' | Where-Object { $_ -and $_ -ne 'C:\Program Files\LLVM\bin' }) -join ';'
$env:Path = "$sdkCmake;" + $env:Path + ";C:\Program Files\Git\usr\bin"
$env:VCPKG_ROOT = "$repo\build-vcpkg"
$env:GAME_VERSION = $Version

Set-Location $repo
if (Test-Path "$build\CMakeCache.txt") { Remove-Item "$build\CMakeCache.txt" -Force }
$cmakeArgs = @(
    "-S", ".", "-B", $build, "-G", "Ninja",
    "-DCMAKE_BUILD_TYPE=Release",
    "-DCMAKE_C_COMPILER=clang-cl", "-DCMAKE_CXX_COMPILER=clang-cl",
    "-DCMAKE_CXX_FLAGS:STRING=-Xclang -fexceptions -Xclang -fcxx-exceptions",
    "-DPATCHES_C_COMPILER=$mipsLlvm/clang.exe".Replace('\','/'),
    "-DPATCHES_LD=$mipsLlvm/ld.lld.exe".Replace('\','/'),
    "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake".Replace('\','/'),
    "-DVCPKG_TARGET_TRIPLET=x64-windows-static-md",
    "-DSK1_ENABLE_JP=OFF"
)
& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed ($LASTEXITCODE)" }
cmake --build $build --target SnowboardKidsRecompiled
if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)" }

# Package like upstream's SnowboardKidsRecompiled-Windows.zip.
$stage = "$build\package"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory $stage | Out-Null
foreach ($f in "SnowboardKidsRecompiled.exe", "crashpad_handler.exe", "dxcompiler.dll", "dxil.dll", "SDL2.dll", "recompcontrollerdb.txt") {
    Copy-Item "$build\$f" $stage
}
Copy-Item "$build\assets" "$stage\assets" -Recurse
New-Item -ItemType Directory "$stage\licenses" | Out-Null
Copy-Item "$build\licenses\crashpad" "$stage\licenses\crashpad" -Recurse
$zip = "$build\SnowboardKidsRecompiled-Windows.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path "$stage\*" -DestinationPath $zip
Write-Host "Packaged: $zip ($([math]::Round((Get-Item $zip).Length/1MB,1)) MB)"
