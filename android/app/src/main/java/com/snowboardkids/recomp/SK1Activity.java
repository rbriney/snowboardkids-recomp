package com.snowboardkids.recomp;

import android.hardware.input.InputManager;
import android.os.Bundle;
import android.util.Log;
import android.view.InputDevice;
import android.view.View;
import android.view.ViewGroup;

import org.libsdl.app.SDLActivity;

import java.io.File;

public class SK1Activity extends SDLActivity {
    private static final String TAG = "SK1";

    private static SK1Activity instance;
    private SK1TouchControls touchControls;

    public static SK1Activity getInstance() {
        return instance;
    }

    @Override
    protected String[] getLibraries() {
        return new String[] { "SnowboardKidsRecompiled" };
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        instance = this;
        super.onCreate(savedInstanceState);

        try {
            File internal = getFilesDir();
            File assets = new File(internal, "assets");
            File external = getExternalFilesDir(null);
            if (external == null) {
                external = internal;
            }

            Log.i(TAG, "Extracting assets to " + assets.getAbsolutePath());
            SK1AssetCopier.extractAssets(this, assets);

            Log.i(TAG, "internal=" + internal + " external=" + external);
            SK1NativeBridge.nativeSetPaths(
                    internal.getAbsolutePath(),
                    assets.getAbsolutePath(),
                    external.getAbsolutePath());
        } catch (Exception e) {
            Log.e(TAG, "Failed to set up Android paths", e);
        }

        // On-screen controls sit above the SDL surface; hidden until a game starts (the launcher uses touch directly).
        if (mLayout != null) {
            touchControls = new SK1TouchControls(this);
            touchControls.setVisibility(View.GONE);
            mLayout.addView(touchControls, new ViewGroup.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        }
    }

    /** Shows the on-screen controls; they start collapsed to the toggle if a physical gamepad is connected. */
    void showTouchControls() {
        if (touchControls == null) {
            return;
        }
        boolean gamepad = isPhysicalGamepadConnected();
        Log.i(TAG, "Game started; physical gamepad connected=" + gamepad);
        touchControls.setControlsHidden(gamepad);
        touchControls.setVisibility(View.VISIBLE);
        touchControls.bringToFront();
    }

    private boolean isPhysicalGamepadConnected() {
        InputManager inputManager = (InputManager) getSystemService(INPUT_SERVICE);
        if (inputManager == null) {
            return false;
        }
        for (int id : inputManager.getInputDeviceIds()) {
            InputDevice device = inputManager.getInputDevice(id);
            if (device == null || device.isVirtual()) {
                continue;
            }
            int sources = device.getSources();
            if ((sources & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
                    || (sources & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK) {
                return true;
            }
        }
        return false;
    }

    @Override
    protected void onDestroy() {
        if (instance == this) {
            instance = null;
        }
        super.onDestroy();
    }
}
