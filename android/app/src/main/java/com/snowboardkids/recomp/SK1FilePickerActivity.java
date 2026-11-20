package com.snowboardkids.recomp;

import android.app.Activity;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;

/**
 * Thin wrapper around the Storage Access Framework document picker. Picked
 * files are copied into the app's external files directory so the native code
 * receives a real filesystem path (SAF content:// URIs cannot be opened by the
 * native recomp runtime).
 */
public class SK1FilePickerActivity extends Activity {
    private static final int REQ_PICK = 0x5346;
    private static final String TAG = "SK1FilePicker";

    private static int pendingId = -1;
    private static boolean pendingMulti = false;

    public static void launch(Activity from, int callbackId, boolean multi) {
        pendingId = callbackId;
        pendingMulti = multi;
        Intent intent = new Intent(from, SK1FilePickerActivity.class);
        from.startActivity(intent);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        Intent picker = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        picker.addCategory(Intent.CATEGORY_OPENABLE);
        picker.setType("*/*");
        picker.putExtra(Intent.EXTRA_MIME_TYPES, new String[]{
                "application/octet-stream",
                "application/zip",
                "application/x-7z-compressed",
                "*/*"
        });
        if (pendingMulti) {
            picker.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
        }
        startActivityForResult(picker, REQ_PICK);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != REQ_PICK) {
            finish();
            return;
        }

        boolean multi = pendingMulti;
        int id = pendingId;
        if (resultCode != RESULT_OK || data == null) {
            deliver(id, multi, null);
            finish();
            return;
        }

        try {
            ArrayList<Uri> uris = new ArrayList<>();
            if (data.getClipData() != null) {
                for (int i = 0; i < data.getClipData().getItemCount(); i++) {
                    uris.add(data.getClipData().getItemAt(i).getUri());
                }
            } else if (data.getData() != null) {
                uris.add(data.getData());
            }

            ArrayList<String> paths = new ArrayList<>();
            for (Uri uri : uris) {
                paths.add(copyToInternal(uri));
            }
            deliver(id, multi, paths);
        } catch (Exception e) {
            android.util.Log.e(TAG, "Failed to copy picked files", e);
            deliver(id, multi, null);
        }
        finish();
    }

    private void deliver(int id, boolean multi, ArrayList<String> paths) {
        if (multi) {
            if (paths == null) {
                SK1NativeBridge.nativeOnFilePickedMultiple(id, false, null);
            } else {
                SK1NativeBridge.nativeOnFilePickedMultiple(id, true, paths.toArray(new String[0]));
            }
        } else {
            if (paths == null || paths.isEmpty()) {
                SK1NativeBridge.nativeOnFilePicked(id, false, null);
            } else {
                SK1NativeBridge.nativeOnFilePicked(id, true, paths.get(0));
            }
        }
    }

    private String copyToInternal(Uri uri) throws Exception {
        String name = queryName(uri);
        if (name == null || name.isEmpty()) {
            name = "picked_" + System.currentTimeMillis();
        }
        File dir = getExternalFilesDir("incoming");
        if (dir == null) {
            dir = new File(getFilesDir(), "incoming");
        }
        dir.mkdirs();
        File out = new File(dir, name);
        try (InputStream in = getContentResolver().openInputStream(uri);
             OutputStream os = new FileOutputStream(out)) {
            byte[] buf = new byte[8192];
            int n;
            while ((n = in.read(buf)) > 0) {
                os.write(buf, 0, n);
            }
        }
        return out.getAbsolutePath();
    }

    private String queryName(Uri uri) {
        try (Cursor c = getContentResolver().query(uri, null, null, null, null)) {
            if (c != null && c.moveToFirst()) {
                int idx = c.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                if (idx >= 0) {
                    return c.getString(idx);
                }
            }
        } catch (Exception ignored) {
        }
        return null;
    }
}
