// Android platform glue. Adapted from the Snowboard Kids 2 Android port
// (kardeiro/snowboardkids2-recomp-Android, GPL-3.0).
#include "sk1_android.h"

#include <SDL.h>
#include <SDL_system.h>
#include <android/log.h>

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>

namespace {
    JavaVM* g_java_vm = nullptr;
    std::filesystem::path g_internal_root;
    std::filesystem::path g_asset_root;
    std::filesystem::path g_external_root;
    jclass g_bridge_class = nullptr;
    std::atomic<SDL_Joystick*> g_touch_joystick{nullptr};

    std::mutex g_dialog_mutex;
    std::condition_variable g_dialog_cv;
    std::map<int, std::shared_ptr<sk1android::file_dialog_result>> g_pending;
    int g_next_dialog_id = 1;
}

// Hooks expected by the Android Plume (Vulkan) patch. Custom Vulkan drivers (Mesa Turnip via
// libadrenotools) only work on Adreno GPUs, so this port always uses the system driver.
extern "C" void* AndroidGetCustomVulkanLoader() {
    return nullptr;
}

extern "C" void AndroidMarkVulkanStartupSuccessful() {
    __android_log_print(ANDROID_LOG_INFO, "SK1", "Vulkan startup successful");
}

namespace sk1android {
    void set_bridge_class(JNIEnv* env, jclass bridge_class) {
        if (g_bridge_class == nullptr && bridge_class != nullptr) {
            g_bridge_class = static_cast<jclass>(env->NewGlobalRef(bridge_class));
        }
    }

    void set_paths(const std::filesystem::path& internal_root,
                   const std::filesystem::path& asset_root,
                   const std::filesystem::path& external_root) {
        g_internal_root = internal_root;
        g_asset_root = asset_root;
        g_external_root = external_root;
        setenv("APP_FOLDER_PATH", external_root.string().c_str(), 1);
        // Renderer diagnostics: create these marker files in the external folder to enable.
        for (const char* flag : { "RT64_UBERSHADERS_ONLY", "RT64_UBERSHADERS_VISIBLE", "RT64_DISABLE_RAW_TMEM" }) {
            if (std::filesystem::exists(external_root / flag)) {
                setenv(flag, "1", 1);
                __android_log_print(ANDROID_LOG_INFO, "SK1", "diagnostic flag %s enabled", flag);
            }
        }
        __android_log_print(ANDROID_LOG_INFO, "SK1", "paths: internal=%s assets=%s external=%s",
                            internal_root.c_str(), asset_root.c_str(), external_root.c_str());
    }

    const std::filesystem::path& asset_root() {
        return g_asset_root;
    }

    const std::filesystem::path& internal_root() {
        return g_internal_root;
    }

    const std::filesystem::path& external_root() {
        return g_external_root;
    }

    JNIEnv* get_env() {
        if (g_java_vm == nullptr) {
            // SDL2 owns JNI_OnLoad on Android, so capture the JavaVM through it.
            JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
            if (env != nullptr) {
                env->GetJavaVM(&g_java_vm);
            }
        }
        if (g_java_vm == nullptr) {
            return nullptr;
        }
        JNIEnv* env = nullptr;
        jint res = g_java_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
        if (res == JNI_EDETACHED) {
            g_java_vm->AttachCurrentThread(&env, nullptr);
        }
        return env;
    }

    void open_file_dialog(bool multi, file_dialog_result& out_result) {
        JNIEnv* env = get_env();
        if (env == nullptr || g_bridge_class == nullptr) {
            out_result.success = false;
            out_result.done.store(true);
            return;
        }

        std::shared_ptr<file_dialog_result> result;
        int id;
        {
            std::lock_guard<std::mutex> lock(g_dialog_mutex);
            id = g_next_dialog_id++;
            result = std::make_shared<file_dialog_result>();
            g_pending[id] = result;
        }

        const char* method_name = multi ? "openFileDialogMultiple" : "openFileDialog";
        jmethodID method = env->GetStaticMethodID(g_bridge_class, method_name, "(I)V");
        if (method != nullptr) {
            env->CallStaticVoidMethod(g_bridge_class, method, static_cast<jint>(id));
        } else {
            env->ExceptionClear();
            result->done.store(true);
        }

        std::unique_lock<std::mutex> lock(g_dialog_mutex);
        g_dialog_cv.wait(lock, [&] { return result->done.load(); });
        out_result.success = result->success;
        out_result.path = result->path;
        out_result.paths = result->paths;
        out_result.done.store(true);
        g_pending.erase(id);
    }

    void deliver_file_dialog(int id, bool success, const std::filesystem::path& path) {
        {
            std::lock_guard<std::mutex> lock(g_dialog_mutex);
            auto it = g_pending.find(id);
            if (it != g_pending.end()) {
                it->second->success = success;
                it->second->path = path;
                it->second->done.store(true);
            }
        }
        g_dialog_cv.notify_all();
    }

    void deliver_file_dialog_multiple(int id, bool success, const std::list<std::filesystem::path>& paths) {
        {
            std::lock_guard<std::mutex> lock(g_dialog_mutex);
            auto it = g_pending.find(id);
            if (it != g_pending.end()) {
                it->second->success = success;
                it->second->paths = paths;
                it->second->done.store(true);
            }
        }
        g_dialog_cv.notify_all();
    }

    void on_game_started() {
        if (g_touch_joystick.load() == nullptr) {
            // 6 axes and 15 buttons give SDL's standard virtual gamepad mapping: axes LEFTX, LEFTY, RIGHTX, RIGHTY,
            // TRIGGERLEFT, TRIGGERRIGHT and buttons in SDL_GameControllerButton order up to DPAD_RIGHT.
            const int device_index = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,
                                                               SDL_CONTROLLER_AXIS_MAX, SDL_CONTROLLER_BUTTON_DPAD_RIGHT + 1, 0);
            if (device_index < 0) {
                __android_log_print(ANDROID_LOG_ERROR, "SK1", "touch controller attach failed: %s", SDL_GetError());
            } else {
                SDL_Joystick* joystick = SDL_JoystickOpen(device_index);
                if (joystick != nullptr) {
                    // Triggers map to 0 at the bottom of the axis range, so they must rest at -32768.
                    SDL_JoystickSetVirtualAxis(joystick, SDL_CONTROLLER_AXIS_TRIGGERLEFT, -32768);
                    SDL_JoystickSetVirtualAxis(joystick, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, -32768);
                    g_touch_joystick.store(joystick);
                    __android_log_print(ANDROID_LOG_INFO, "SK1", "touch controller attached (device %d)", device_index);
                }
            }
        }

        JNIEnv* env = get_env();
        if (env != nullptr && g_bridge_class != nullptr) {
            jmethodID method = env->GetStaticMethodID(g_bridge_class, "onGameStarted", "()V");
            if (method != nullptr) {
                env->CallStaticVoidMethod(g_bridge_class, method);
            } else {
                env->ExceptionClear();
            }
        }
    }

    void set_touch_button(int sdl_button, bool pressed) {
        SDL_Joystick* joystick = g_touch_joystick.load();
        if (joystick != nullptr) {
            SDL_JoystickSetVirtualButton(joystick, sdl_button, pressed ? SDL_PRESSED : SDL_RELEASED);
        }
    }

    void set_touch_axis(int sdl_axis, int value) {
        SDL_Joystick* joystick = g_touch_joystick.load();
        if (joystick != nullptr) {
            SDL_JoystickSetVirtualAxis(joystick, sdl_axis, static_cast<Sint16>(SDL_clamp(value, -32768, 32767)));
        }
    }
}
