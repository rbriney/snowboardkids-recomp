# Package the APK from the native build (run build-android-native.ps1 first), and optionally install it.
# Usage: powershell -ExecutionPolicy Bypass -File tools\build-apk.ps1 [-Install]
param([switch]$Install)
$ErrorActionPreference = "Stop"

$repo = Split-Path $PSScriptRoot -Parent
$env:Path = [Environment]::GetEnvironmentVariable("Path","Machine") + ";" + [Environment]::GetEnvironmentVariable("Path","User")
$env:ANDROID_HOME = "D:\Android\Sdk"
$env:ANDROID_SDK_ROOT = "D:\Android\Sdk"
$env:JAVA_HOME = [Environment]::GetEnvironmentVariable("JAVA_HOME","Machine")
$env:SK1_NATIVE_BUILD_DIR = "$repo\build-android"

Set-Location "$repo\android"
& .\gradlew.bat --no-daemon assembleDebug
if ($LASTEXITCODE -ne 0) { throw "Gradle build failed ($LASTEXITCODE)" }
$apk = Get-ChildItem "$repo\android\app\build\outputs\apk\debug\*.apk" | Select-Object -First 1
Write-Host "APK: $($apk.FullName) ($([math]::Round($apk.Length/1MB,1)) MB)"

if ($Install) {
    & adb install -r $apk.FullName
    if ($LASTEXITCODE -ne 0) { throw "adb install failed ($LASTEXITCODE)" }
    Write-Host "Installed."
}
