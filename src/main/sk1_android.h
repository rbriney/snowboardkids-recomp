#pragma once

// Android platform glue: app paths and the Storage Access Framework file picker.
// Adapted from the Snowboard Kids 2 Android port (kardeiro/snowboardkids2-recomp-Android, GPL-3.0).

#include <atomic>
#include <filesystem>
#include <list>

#include <jni.h>

namespace sk1android {
    // Set once at startup from Java (SK1NativeBridge.nativeSetPaths). Also exports
    // APP_FOLDER_PATH, which RT64's user path detection reads.
    void set_paths(const std::filesystem::path& internal_root,
                   const std::filesystem::path& asset_root,
                   const std::filesystem::path& external_root);

    // Caches a global reference to the SK1NativeBridge class. JNIEnv::FindClass does not
    // work from the SDL main thread (a native pthread using the bootstrap class loader),
    // so the class is captured from a Java-attached thread at startup.
    void set_bridge_class(JNIEnv* env, jclass bridge_class);

    // Directory holding the assets extracted from the APK (already the "assets" folder).
    const std::filesystem::path& asset_root();
    // App-private internal storage.
    const std::filesystem::path& internal_root();
    // App-specific external storage: saves, config, mods.
    const std::filesystem::path& external_root();

    struct file_dialog_result {
        std::atomic<bool> done{false};
        bool success = false;
        std::filesystem::path path;
        std::list<std::filesystem::path> paths;
    };

    // Blocks the calling thread until the user picks or cancels through the Storage
    // Access Framework. The picked file is copied into app storage and that path returned.
    void open_file_dialog(bool multi, file_dialog_result& out_result);

    // Called from Java (UI thread) to deliver a picker result.
    void deliver_file_dialog(int id, bool success, const std::filesystem::path& path);
    void deliver_file_dialog_multiple(int id, bool success, const std::list<std::filesystem::path>& paths);

    // Attaches to the JVM if needed and returns the current thread's JNIEnv.
    JNIEnv* get_env();

    // Called on the game thread when a game starts: attaches the virtual controller driven by the on-screen
    // touch controls and asks Java to show them.
    void on_game_started();

    // On-screen touch controls feed an SDL virtual game controller (standard gamepad layout).
    void set_touch_button(int sdl_button, bool pressed);
    void set_touch_axis(int sdl_axis, int value);
}
