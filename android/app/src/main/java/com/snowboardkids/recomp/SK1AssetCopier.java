package com.snowboardkids.recomp;

import android.content.Context;
import android.content.res.AssetManager;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;

/**
 * Copies the packaged assets (fonts, rcss, images) from the APK to the app's
 * internal storage so the native code can access them through the regular
 * filesystem.
 */
public final class SK1AssetCopier {
    private SK1AssetCopier() {}

    public static void extractAssets(Context context, File destDir) {
        AssetManager am = context.getAssets();
        try {
            if (!destDir.exists()) {
                destDir.mkdirs();
            }
            copyAssetTree(am, "", destDir);
        } catch (Exception e) {
            throw new RuntimeException("Failed to extract assets", e);
        }
    }

    private static void copyAssetTree(AssetManager am, String prefix, File destDir) throws Exception {
        String[] children = am.list(prefix);
        if (children == null) {
            return;
        }
        for (String child : children) {
            String rel = prefix.isEmpty() ? child : prefix + "/" + child;
            File out = new File(destDir, rel);
            String[] sub = am.list(rel);
            if (sub != null && sub.length > 0) {
                out.mkdirs();
                copyAssetTree(am, rel, destDir);
            } else {
                try (InputStream in = am.open(rel);
                     OutputStream os = new FileOutputStream(out)) {
                    byte[] buf = new byte[8192];
                    int n;
                    while ((n = in.read(buf)) > 0) {
                        os.write(buf, 0, n);
                    }
                }
            }
        }
    }
}
