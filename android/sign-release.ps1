# Aligns and signs the unsigned release APK (from `gradlew assembleRelease`). apksigner prompts for the keystore
# password; nothing is stored in this script.
#
# Usage: powershell -ExecutionPolicy Bypass -File android\sign-release.ps1 -Keystore D:\path\to\release.jks
param(
    [Parameter(Mandatory = $true)][string]$Keystore,
    [string]$Alias = "sbk-release",
    [string]$Output = ""
)
$ErrorActionPreference = "Stop"
# $PSScriptRoot is not yet set while Windows PowerShell 5.1 evaluates parameter defaults, so resolve it here.
if (-not $Output) { $Output = "$PSScriptRoot\app\build\outputs\apk\release\SnowboardKidsRecompiled-Android-ARM64.apk" }

$sdk = if ($env:ANDROID_HOME) { $env:ANDROID_HOME } else { $env:ANDROID_SDK_ROOT }
$buildTools = Join-Path $sdk "build-tools\34.0.0"
$unsigned = "$PSScriptRoot\app\build\outputs\apk\release\app-release-unsigned.apk"
$aligned = "$PSScriptRoot\app\build\outputs\apk\release\app-release-aligned.apk"
if (-not (Test-Path $unsigned)) { throw "$unsigned not found; run 'gradlew assembleRelease' first." }

& "$buildTools\zipalign.exe" -p -f 4 $unsigned $aligned
if ($LASTEXITCODE -ne 0) { throw "zipalign failed ($LASTEXITCODE)" }

& "$buildTools\apksigner.bat" sign --ks $Keystore --ks-key-alias $Alias `
    --v1-signing-enabled true --v2-signing-enabled true --v3-signing-enabled true --out $Output $aligned
if ($LASTEXITCODE -ne 0) { throw "apksigner sign failed ($LASTEXITCODE)" }
Remove-Item $aligned

& "$buildTools\apksigner.bat" verify --verbose $Output
if ($LASTEXITCODE -ne 0) { throw "apksigner verify failed ($LASTEXITCODE)" }
Write-Host "Signed: $Output"
