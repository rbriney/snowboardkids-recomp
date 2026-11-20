# Creates the release signing key (one time). keytool prompts for the passwords; nothing is stored in this script.
#
# KEEP THE KEYSTORE AND ITS PASSWORD SAFE AND BACKED UP. Android only installs an update over an existing install
# if it is signed with the same key, so losing it means users must uninstall (losing their app data) to update.
#
# Usage: powershell -ExecutionPolicy Bypass -File android\create-release-key.ps1 -Keystore D:\path\to\release.jks
param(
    [Parameter(Mandatory = $true)][string]$Keystore,
    [string]$Alias = "sbk-release"
)
$ErrorActionPreference = "Stop"

if (Test-Path $Keystore) { throw "$Keystore already exists; refusing to overwrite a signing key." }
$keytool = Join-Path $env:JAVA_HOME "bin\keytool.exe"
if (-not (Test-Path $keytool)) { $keytool = "keytool" }

New-Item -ItemType Directory -Force (Split-Path -Parent $Keystore) | Out-Null
# The certificate's name is embedded in every signed APK, so it names the project rather than a person.
& $keytool -genkeypair -v -keystore $Keystore -alias $Alias -keyalg RSA -keysize 4096 -validity 10000 `
    -dname "CN=Snowboard Kids Recompiled Android"
if ($LASTEXITCODE -ne 0) { throw "keytool failed ($LASTEXITCODE)" }
Write-Host "Created $Keystore (alias '$Alias'). Back it up now, together with its password."
