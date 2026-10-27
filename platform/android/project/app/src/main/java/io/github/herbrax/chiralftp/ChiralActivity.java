package io.github.herbrax.chiralftp;

import android.Manifest;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Rect;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.view.View;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

import org.libsdl.app.SDLActivity;

/* chiral-ftp's activity: SDL's, plus asking for the storage the file
   manager and the FTP server need. On Android 11 and up that is "All files
   access", granted on its own Settings page; below, the two storage
   permissions. Asked once per launch until granted; the app works without
   it, showing only what Android lets any app see. The first-launch screen
   in the mockups replaces this. */
public class ChiralActivity extends SDLActivity {
    private static final int STORAGE_REQUEST = 1;
    private boolean asked = false;

    /* app/main.cpp's main() is the program's own, as on every platform,
       not renamed SDL_main by SDL's header: start that. */
    @Override
    protected String getMainFunction() {
        return "main";
    }

    /* main() runs once per process, as on every other platform: its
       statics (the UI, the queues, a Quit still queued from the last run)
       aren't made to start over, and the last run's threads may still be
       winding down. Android, though, can create the activity again in a
       process that has already run it -- after the old one was destroyed,
       or while it is being replaced -- and SDL would run main() a second
       time there, which quit at once and crashed. So a second activity in
       one process starts the app over in a new one instead. */
    private static boolean mainStarted = false;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        if (mainStarted) {
            startActivity(Intent.makeRestartActivityTask(getComponentName()));
            Runtime.getRuntime().exit(0);
            return;
        }
        mainStarted = true;
        askForStorage();
        final View root = getWindow().getDecorView();
        root.addOnLayoutChangeListener((v, l, t, r, b, ol, ot, or, ob) -> claimSideEdges(v));
    }

    /* Called from the native side (platform/android/backend.cpp,
       to_background): Back on the first screen. The task goes behind the
       home screen rather than ending, so the servers keep running. */
    public void toBackground() {
        runOnUiThread(() -> moveTaskToBack(true));
    }

    /* Called from the native side (platform/android/backend.cpp,
       open_file): a file on this device, to the app that opens its kind
       (how 0), to the system's "open with" chooser (1), or to its share
       sheet (2). False when no app can take it. */
    public boolean openFile(byte[] pathBytes, int how) {
        final File file = new File(new String(pathBytes, StandardCharsets.UTF_8));
        if (!file.isFile()) {
            return false;
        }
        final Uri uri = ChiralFileProvider.uriFor(this, file);
        final String mime = ChiralFileProvider.mimeOf(file.getName());
        Intent intent;
        if (how == 2) {
            intent = new Intent(Intent.ACTION_SEND);
            intent.setType(mime);
            intent.putExtra(Intent.EXTRA_STREAM, uri);
        } else {
            intent = new Intent(Intent.ACTION_VIEW);
            intent.setDataAndType(uri, mime);
        }
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        if (how == 0 && intent.resolveActivity(getPackageManager()) == null) {
            return false;
        }
        final Intent start = how == 0 ? intent : Intent.createChooser(intent, file.getName());
        start.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        runOnUiThread(() -> {
            try {
                startActivity(start);
            } catch (Exception ignored) {
            }
        });
        return true;
    }

    /* Sideways, a swipe in from the middle of the left or right edge brings
       chiral-ftp's tabs in (app/ui_mobile.inc). Those edges are also
       Android's back gesture, so a band of each -- 200dp is the most
       Android grants per edge -- is claimed for the app. Upright, the tabs
       are always on screen and the back gesture keeps the whole edge. */
    private void claimSideEdges(View v) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) {
            return;
        }
        final int w = v.getWidth(), h = v.getHeight();
        final float dp = getResources().getDisplayMetrics().density;
        final List<Rect> rects = new ArrayList<>();
        if (w > h) {
            final int band = (int) (200 * dp), edge = (int) (32 * dp), top = (h - band) / 2;
            rects.add(new Rect(0, top, edge, top + band));
            rects.add(new Rect(w - edge, top, w, top + band));
        }
        v.setSystemGestureExclusionRects(rects);
    }

    private void askForStorage() {
        if (asked) {
            return;
        }
        asked = true;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            if (Environment.isExternalStorageManager()) {
                return;
            }
            try {
                Intent i = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                                      Uri.parse("package:" + getPackageName()));
                startActivity(i);
            } catch (Exception e) {
                /* Some TVs have no such page: adb appops is the way there
                   (docs/ANDROID-ROADMAP.md, section 5.3). */
                try {
                    startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
                } catch (Exception ignored) {
                }
            }
        } else if (checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[] {Manifest.permission.READ_EXTERNAL_STORAGE,
                                             Manifest.permission.WRITE_EXTERNAL_STORAGE}, STORAGE_REQUEST);
        }
    }
}
