# Port build, release and test scripts (Windows)

PowerShell scripts for building the Android and Windows versions of this port, and for the unattended netplay and
determinism tests. Run them from anywhere; they find the repository from their own location, e.g.

```powershell
powershell -ExecutionPolicy Bypass -File tools\build-windows.ps1
```

| Script | What it does |
|---|---|
| `build-windows.ps1 [-Reconfigure]` | Windows build in `build-windows\` (for testing) |
| `build-windows-release.ps1 -Version X` | Clean Windows release build and `SnowboardKidsRecompiled-Windows.zip` |
| `build-android-native.ps1 [-Reconfigure]` | `libSnowboardKidsRecompiled.so` for arm64 in `build-android\` |
| `build-apk.ps1 [-Install]` | Debug APK from the native build, optionally installed with adb |
| `replay-test.ps1` | Replays a determinism recording and prints the report |
| `netplay-test.ps1` | Runs several copies against each other over 127.0.0.1 from a recording (see the script's header) |

Tool locations (Android SDK and NDK, the MIPS-capable LLVM, Visual Studio Build Tools) are set near the top of each
script; change them to match your machine. Release signing is in `android\sign-release.ps1`.

When releasing, bump the version in `android\app\build.gradle` and `GAME_VERSION` in `build-android-native.ps1`, then
run `build-android-native.ps1 -Reconfigure`. After adding or changing a file in `patches\`, rebuild with `-Reconfigure`.
