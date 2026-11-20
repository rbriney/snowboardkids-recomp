// JNI entry points called from Java (com.snowboardkids.recomp.SK1NativeBridge).
// JNI_OnLoad is intentionally not defined: SDL2's Android backend provides it and is
// statically linked into this library.
// Adapted from the Snowboard Kids 2 Android port (kardeiro/snowboardkids2-recomp-Android, GPL-3.0).
#include <jni.h>

#include <filesystem>
#include <list>
#include <string>

#include "sk1_android.h"

namespace {
    std::filesystem::path from_jstring(JNIEnv* env, jstring str) {
        if (str == nullptr) {
            return {};
        }
        const char* utf = env->GetStringUTFChars(str, nullptr);
        std::filesystem::path result{utf != nullptr ? utf : ""};
        if (utf != nullptr) {
            env->ReleaseStringUTFChars(str, utf);
        }
        return result;
    }

    std::list<std::filesystem::path> from_jarray(JNIEnv* env, jobjectArray arr) {
        std::list<std::filesystem::path> result;
        if (arr == nullptr) {
            return result;
        }
        jsize count = env->GetArrayLength(arr);
        for (jsize i = 0; i < count; i++) {
            auto str = static_cast<jstring>(env->GetObjectArrayElement(arr, i));
            result.push_back(from_jstring(env, str));
            env->DeleteLocalRef(str);
        }
        return result;
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_snowboardkids_recomp_SK1NativeBridge_nativeSetPaths(JNIEnv* env, jclass bridge_class,
                                                             jstring internal_root, jstring asset_root,
                                                             jstring external_root) {
    sk1android::set_bridge_class(env, bridge_class);
    sk1android::set_paths(from_jstring(env, internal_root), from_jstring(env, asset_root),
                          from_jstring(env, external_root));
}

extern "C" JNIEXPORT void JNICALL
Java_com_snowboardkids_recomp_SK1NativeBridge_nativeOnFilePicked(JNIEnv* env, jclass, jint id,
                                                                 jboolean success, jstring path) {
    sk1android::deliver_file_dialog(static_cast<int>(id), success == JNI_TRUE, from_jstring(env, path));
}

extern "C" JNIEXPORT void JNICALL
Java_com_snowboardkids_recomp_SK1NativeBridge_nativeOnFilePickedMultiple(JNIEnv* env, jclass, jint id,
                                                                         jboolean success, jobjectArray paths) {
    sk1android::deliver_file_dialog_multiple(static_cast<int>(id), success == JNI_TRUE, from_jarray(env, paths));
}
