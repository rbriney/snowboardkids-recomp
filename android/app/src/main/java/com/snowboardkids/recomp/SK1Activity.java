package com.snowboardkids.recomp;

import android.app.Activity;
import android.os.Bundle;
import android.util.Log;

import org.libsdl.app.SDLActivity;

import java.io.File;

public class SK1Activity extends SDLActivity {
    private static final String TAG = "SK1";

    private static SK1Activity instance;

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
    }

    @Override
    protected void onDestroy() {
        if (instance == this) {
            instance = null;
        }
        super.onDestroy();
    }
}
