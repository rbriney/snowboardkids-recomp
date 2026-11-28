# Configure and build libSnowboardKidsRecompiled.so for Android (arm64-v8a) with the NDK.
# Usage: powershell -ExecutionPolicy Bypass -File tools\build-android-native.ps1 [-Reconfigure]
param([switch]$Reconfigure)
$ErrorActionPreference = "Stop"

$repo  = Split-Path $PSScriptRoot -Parent
$build = "$repo\build-android"
$ndk   = "D:\Android\Sdk\ndk\26.3.11579264"
$sdkCmake = "D:\Android\Sdk\cmake\3.31.6\bin"
$mipsLlvm = "D:\dev\tools\LLVM-19.1.3-Windows-X64\bin"

$env:Path = [Environment]::GetEnvironmentVariable("Path","Machine") + ";" + [Environment]::GetEnvironmentVariable("Path","User")
# VS developer environment: host tools (file_to_c) are built with clang-cl during configure.
& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
$env:Path = ($env:Path -split ';' | Where-Object { $_ -and $_ -ne 'C:\Program Files\LLVM\bin' }) -join ';'
$env:Path = "$sdkCmake;" + $env:Path + ";C:\Program Files\Git\usr\bin"
if (-not $env:GAME_VERSION) { $env:GAME_VERSION = "0.1.6" }

Set-Location $repo
if ($Reconfigure -and (Test-Path "$build\CMakeCache.txt")) { Remove-Item "$build\CMakeCache.txt" -Force }
if ($Reconfigure -or -not (Test-Path "$build\build.ninja")) {
    $cmakeArgs = @(
        "-S", ".", "-B", $build, "-G", "Ninja",
        "-DCMAKE_MAKE_PROGRAM=$sdkCmake/ninja.exe".Replace('\','/'),
        "-DCMAKE_TOOLCHAIN_FILE=$ndk/build/cmake/android.toolchain.cmake".Replace('\','/'),
        "-DANDROID_ABI=arm64-v8a",
        "-DANDROID_PLATFORM=android-26",
        "-DANDROID_STL=c++_static",
        "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_C_FLAGS=-g",
        "-DCMAKE_CXX_FLAGS=-g",
        "-DPATCHES_C_COMPILER=$mipsLlvm/clang.exe".Replace('\','/'),
        "-DPATCHES_LD=$mipsLlvm/ld.lld.exe".Replace('\','/'),
        "-DSK1_ENABLE_JP=OFF"
    )
    Write-Host "cmake $($cmakeArgs -join ' | ')"
    & cmake @cmakeArgs
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed ($LASTEXITCODE)" }
}
cmake --build $build --target SnowboardKidsRecompiled StageRuntimeAssets
if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)" }
Write-Host "Built: $build\libSnowboardKidsRecompiled.so"
