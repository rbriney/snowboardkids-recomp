# Android port

Builds Snowboard Kids: Recompiled as an Android APK (arm64-v8a, Android 8.0+, Vulkan GPU).

Based on the Snowboard Kids 2 Android port by kardeiro/deivid22srk
([kardeiro/snowboardkids2-recomp-Android](https://github.com/kardeiro/snowboardkids2-recomp-Android), GPL-3.0),
which uses the same RT64 / N64ModernRuntime / RecompFrontend libraries.

Status: playable on a Pixel 9 (Mali-G715). Custom Vulkan drivers (Turnip via libadrenotools) are not included;
the system driver is used.

Controls: on-screen touch controls (analog stick, A, B, Z, Start) appear when a game starts. They drive an SDL
virtual game controller, so the game sees them like a physical gamepad. A toggle at the top centre hides or
shows them; they start hidden when a physical controller is connected. Bluetooth/USB controllers work too, and
on Android the D-pad also drives the analog stick so D-pad-only pads can steer.

## How it fits together

- The game is built with CMake and the NDK as one shared library, `libSnowboardKidsRecompiled.so`, with SDL2
  and FreeType linked statically (`if (ANDROID)` blocks in the top-level `CMakeLists.txt`).
- `android/` is a Gradle project that packages that library plus the staged runtime assets. Its Java code is
  SDL's `SDLActivity` (from the SDL 2.30.9 source, `org/libsdl/app`) and a few small classes in
  `com/snowboardkids/recomp`:
  - `SK1Activity` extracts the APK assets to internal storage and passes paths to native code.
  - `SK1FilePickerActivity` / `SK1NativeBridge` implement the ROM picker with the Storage Access Framework.
- Native glue is in `src/main/sk1_android*.cpp`.
- The Android changes to the submodules (rt64, Plume, nativefiledialog-extended, zstd, RecompFrontend) are kept
  as patches in `android/patches/` and applied on top of the pinned submodule commits.

## Building (Windows host)

Requirements: Android SDK with NDK 26.3.11579264, CMake 3.31.6 and build-tools 34; JDK 17; Visual Studio 2022
Build Tools (clang-cl, for host tools); the portable LLVM 19.1.3 for the MIPS patch payload (see BUILDING.md);
N64Recomp/RSPRecomp and the generated code from `scripts/generate-code.py` (see BUILDING.md).

1. Apply the submodule patches (once per fresh checkout):

   ```
   git -C lib/rt64 apply ../../android/patches/rt64.patch
   git -C lib/rt64/src/contrib/nativefiledialog-extended apply ../../../../android/patches/rt64-nfd.patch
   git -C lib/rt64/src/contrib/plume apply ../../../../android/patches/rt64-plume.patch
   git -C lib/rt64/src/contrib/zstd apply ../../../../android/patches/rt64-zstd.patch
   git -C lib/RecompFrontend apply ../../android/patches/recompfrontend.patch
   ```

2. Configure and build the native library from a Visual Studio developer shell. Without release tags, set the
   version first (shown on the launcher), e.g. `set GAME_VERSION=0.1.0`:

   ```
   cmake -S . -B build-android -G Ninja ^
     -DCMAKE_TOOLCHAIN_FILE=%ANDROID_HOME%/ndk/26.3.11579264/build/cmake/android.toolchain.cmake ^
     -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 -DANDROID_STL=c++_static ^
     -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS=-g -DCMAKE_CXX_FLAGS=-g ^
     -DPATCHES_C_COMPILER=<llvm-19>/bin/clang.exe -DPATCHES_LD=<llvm-19>/bin/ld.lld.exe ^
     -DSK1_ENABLE_JP=OFF
   cmake --build build-android --target SnowboardKidsRecompiled StageRuntimeAssets
   ```

3. Package and install the APK:

   ```
   cd android
   gradlew assembleDebug
   adb install -r app/build/outputs/apk/debug/app-debug.apk
   ```

`SK1_NATIVE_BUILD_DIR` overrides where Gradle looks for the native build (default `../build-android`).

## Release builds

Release APKs are signed with a release key that stays off the repository. Both scripts let the Java tools
prompt for the password, so it never appears in a file, environment variable or command line.

1. Create the key once (back up the `.jks` file and its password: every update must be signed with the same
   key or Android refuses to install it over the previous version):

   ```
   powershell -ExecutionPolicy Bypass -File android\create-release-key.ps1 -Keystore D:\path\to\release.jks
   ```

2. Build the native library (step 2 above, with `GAME_VERSION` set), then the unsigned, stripped release APK:

   ```
   cd android
   gradlew assembleRelease
   ```

3. Align, sign and verify it, producing `app/build/outputs/apk/release/SnowboardKidsRecompiled-Android-ARM64.apk`:

   ```
   powershell -ExecutionPolicy Bypass -File android\sign-release.ps1 -Keystore D:\path\to\release.jks
   ```

For each release, increase `versionCode` (and `versionName`) in `app/build.gradle`.

## On the device

On first launch, pick your legally obtained US Snowboard Kids ROM (`.z64`) when the launcher asks. Saves, config
and mods live in the app's external files folder (`Android/data/com.snowboardkids.recomp/files`).

Logs: `adb logcat -s SK1 SDL plume RT64 DEBUG`
