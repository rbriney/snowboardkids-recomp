package com.snowboardkids.recomp;

/**
 * Bridge between the Java (UI/picker) side and the native (recomp/Rt64) side.
 * All JNI entry points are implemented in sk1_android_jni.cpp which lives in
 * the main game shared library (libSnowboardKidsRecompiled.so).
 */
public final class SK1NativeBridge {
    private SK1NativeBridge() {}

    public static native void nativeSetPaths(String internalRoot, String assetRoot, String externalRoot);

    public static native void nativeOnFilePicked(int callbackId, boolean success, String path);
    public static native void nativeOnFilePickedMultiple(int callbackId, boolean success, String[] paths);

    // On-screen touch controls -> SDL virtual game controller (SDL_GameControllerButton / SDL_GameControllerAxis).
    public static native void nativeTouchButton(int sdlButton, boolean pressed);
    public static native void nativeTouchAxis(int sdlAxis, int value);

    // Called from native code on the game thread when a game starts.
    public static void onGameStarted() {
        final SK1Activity activity = SK1Activity.getInstance();
        if (activity != null) {
            activity.runOnUiThread(activity::showTouchControls);
        }
    }

    // Called from native code on the SDL thread. Launches the SAF picker on the UI thread.
    public static void openFileDialog(int callbackId) {
        final SK1Activity activity = SK1Activity.getInstance();
        if (activity == null) {
            nativeOnFilePicked(callbackId, false, null);
            return;
        }
        activity.runOnUiThread(() -> SK1FilePickerActivity.launch(activity, callbackId, false));
    }

    public static void openFileDialogMultiple(int callbackId) {
        final SK1Activity activity = SK1Activity.getInstance();
        if (activity == null) {
            nativeOnFilePickedMultiple(callbackId, false, null);
            return;
        }
        activity.runOnUiThread(() -> SK1FilePickerActivity.launch(activity, callbackId, true));
    }
}
