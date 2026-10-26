package io.github.herbrax.chiralftp;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.Context;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;
import android.webkit.MimeTypeMap;

import java.io.File;
import java.io.FileNotFoundException;
import java.util.Locale;

/* Hands a file on this device to another app (Open, Open with, Share), as a
   content:// address: since Android 7 a file:// one makes the other app
   crash. Not exported: another app can read only a file it was given a
   grant for, and only to read it. The address is the file's own path. */
public class ChiralFileProvider extends ContentProvider {
    public static Uri uriFor(Context context, File file) {
        return new Uri.Builder().scheme("content").authority(context.getPackageName() + ".files")
                                .path(file.getAbsolutePath()).build();
    }

    public static String mimeOf(String name) {
        final int dot = name.lastIndexOf('.');
        if (dot >= 0) {
            final String ext = name.substring(dot + 1).toLowerCase(Locale.ROOT);
            final String mime = MimeTypeMap.getSingleton().getMimeTypeFromExtension(ext);
            if (mime != null) {
                return mime;
            }
        }
        return "application/octet-stream";
    }

    private static File fileOf(Uri uri) throws FileNotFoundException {
        final String path = uri.getPath();
        if (path == null) {
            throw new FileNotFoundException("no path");
        }
        final File f = new File(path);
        if (!f.isFile()) {
            throw new FileNotFoundException(path);
        }
        return f;
    }

    @Override
    public boolean onCreate() {
        return true;
    }

    @Override
    public String getType(Uri uri) {
        return mimeOf(uri.getLastPathSegment() == null ? "" : uri.getLastPathSegment());
    }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        if (!"r".equals(mode)) {
            throw new FileNotFoundException("read only");
        }
        return ParcelFileDescriptor.open(fileOf(uri), ParcelFileDescriptor.MODE_READ_ONLY);
    }

    /* The name and size, which other apps ask before they open it. */
    @Override
    public Cursor query(Uri uri, String[] projection, String selection, String[] args, String order) {
        final File f;
        try {
            f = fileOf(uri);
        } catch (FileNotFoundException e) {
            return null;
        }
        final String[] cols = projection != null ? projection
                                                 : new String[] {OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE};
        final MatrixCursor c = new MatrixCursor(cols, 1);
        final Object[] row = new Object[cols.length];
        for (int i = 0; i < cols.length; i++) {
            if (OpenableColumns.DISPLAY_NAME.equals(cols[i])) {
                row[i] = f.getName();
            } else if (OpenableColumns.SIZE.equals(cols[i])) {
                row[i] = f.length();
            }
        }
        c.addRow(row);
        return c;
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        throw new UnsupportedOperationException();
    }

    @Override
    public int delete(Uri uri, String selection, String[] args) {
        throw new UnsupportedOperationException();
    }

    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] args) {
        throw new UnsupportedOperationException();
    }
}
