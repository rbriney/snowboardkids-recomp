# Configure and build the Windows desktop version of Snowboard Kids Recompiled.
# Usage: powershell -ExecutionPolicy Bypass -File tools\build-windows.ps1 [-Reconfigure]
param([switch]$Reconfigure)
$ErrorActionPreference = "Stop"

$repo  = Split-Path $PSScriptRoot -Parent
$build = "$repo\build-windows"
$sdkCmake = "D:\Android\Sdk\cmake\3.31.6\bin"

# Fresh PATH from the registry (picks up tools installed after this shell started).
$env:Path = [Environment]::GetEnvironmentVariable("Path","Machine") + ";" + [Environment]::GetEnvironmentVariable("Path","User")
& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
# Portable LLVM with the MIPS backend, used only for the N64 patch code (same version as upstream CI).
$mipsLlvm = "D:\dev\tools\LLVM-19.1.3-Windows-X64\bin"
# Like upstream CI, keep the system LLVM off PATH so clang-cl comes from Visual Studio.
$env:Path = ($env:Path -split ';' | Where-Object { $_ -and $_ -ne 'C:\Program Files\LLVM\bin' }) -join ';'
# CMake 3.31.6 first; Git's POSIX tools last so they never shadow MSVC's link.exe.
$env:Path = "$sdkCmake;" + $env:Path + ";C:\Program Files\Git\usr\bin"
$env:VCPKG_ROOT = "$repo\build-vcpkg"
if (-not $env:GAME_VERSION) { $env:GAME_VERSION = "0.0.0-android-dev" }

Set-Location $repo
if ($Reconfigure -or -not (Test-Path "$build\build.ninja")) {
    # Arguments go through an array: PowerShell 5.1 mangles inline quoted -D values that contain spaces.
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
    Write-Host "cmake $($cmakeArgs -join ' | ')"
    & cmake @cmakeArgs
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed ($LASTEXITCODE)" }
}
cmake --build $build --target SnowboardKidsRecompiled
if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)" }
Write-Host "Built: $build\SnowboardKidsRecompiled.exe (run it with $build as the working directory; assets are staged there)"
