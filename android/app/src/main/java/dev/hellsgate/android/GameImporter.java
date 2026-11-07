package dev.hellsgate.android;

import android.content.ContentResolver;
import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.provider.DocumentsContract;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Copies an already-extracted Xbox 360 game folder (the one containing default.xex) out of a
 * Storage Access Framework tree into app-private storage, where the native runtime can use plain
 * file paths. The ISO must be extracted first (for example with extract-xiso on a PC).
 */
final class GameImporter {
    interface Progress {
        void onProgress(long copiedBytes, long totalBytes, String currentFile);
    }

    private static final String[] COLUMNS = {
            DocumentsContract.Document.COLUMN_DOCUMENT_ID,
            DocumentsContract.Document.COLUMN_DISPLAY_NAME,
            DocumentsContract.Document.COLUMN_MIME_TYPE,
            DocumentsContract.Document.COLUMN_SIZE,
    };

    private static final int MAX_SEARCH_DEPTH = 3;

    private static final class Entry {
        final String documentId;
        final String name;
        final boolean directory;
        final long size;

        Entry(String documentId, String name, boolean directory, long size) {
            this.documentId = documentId;
            this.name = name;
            this.directory = directory;
            this.size = size;
        }
    }

    private final ContentResolver resolver;
    private final Uri treeUri;

    private GameImporter(Context context, Uri treeUri) {
        this.resolver = context.getContentResolver();
        this.treeUri = treeUri;
    }

    static void importTree(Context context, Uri treeUri, File destination, Progress progress,
                           AtomicBoolean cancelled) throws IOException {
        new GameImporter(context, treeUri).run(destination, progress, cancelled);
    }

    private void run(File destination, Progress progress, AtomicBoolean cancelled)
            throws IOException {
        String rootId = DocumentsContract.getTreeDocumentId(treeUri);
        String gameRootId = findGameRoot(rootId, 0);
        if (gameRootId == null) {
            throw new IOException("default.xex was not found in the selected folder. Pick the "
                    + "extracted game folder (not the ISO).");
        }

        long total = measure(gameRootId);

        File parent = destination.getParentFile();
        if (parent == null) {
            throw new IOException("Invalid destination");
        }
        File staging = new File(parent, destination.getName() + ".importing");
        deleteRecursively(staging);
        if (!staging.mkdirs()) {
            throw new IOException("Cannot create " + staging);
        }

        long[] copied = {0};
        try {
            copyDirectory(gameRootId, staging, total, copied, progress, cancelled);
        } catch (IOException | RuntimeException e) {
            deleteRecursively(staging);
            throw e;
        }
        if (cancelled.get()) {
            deleteRecursively(staging);
            throw new IOException("Import cancelled");
        }

        deleteRecursively(destination);
        if (!staging.renameTo(destination)) {
            throw new IOException("Cannot move imported files into place");
        }
    }

    /** Returns the id of the directory that directly contains default.xex, or null. */
    private String findGameRoot(String directoryId, int depth) throws IOException {
        List<Entry> children = list(directoryId);
        for (Entry child : children) {
            if (!child.directory && child.name.equalsIgnoreCase("default.xex")) {
                return directoryId;
            }
        }
        if (depth >= MAX_SEARCH_DEPTH) {
            return null;
        }
        for (Entry child : children) {
            if (child.directory) {
                String found = findGameRoot(child.documentId, depth + 1);
                if (found != null) {
                    return found;
                }
            }
        }
        return null;
    }

    private long measure(String directoryId) throws IOException {
        long sum = 0;
        for (Entry child : list(directoryId)) {
            sum += child.directory ? measure(child.documentId) : Math.max(child.size, 0);
        }
        return sum;
    }

    private void copyDirectory(String directoryId, File target, long total, long[] copied,
                               Progress progress, AtomicBoolean cancelled) throws IOException {
        for (Entry child : list(directoryId)) {
            if (cancelled.get()) {
                return;
            }
            String safeName = sanitize(child.name);
            File out = new File(target, safeName);
            if (child.directory) {
                if (!out.isDirectory() && !out.mkdirs()) {
                    throw new IOException("Cannot create " + out);
                }
                copyDirectory(child.documentId, out, total, copied, progress, cancelled);
            } else {
                copyFile(child, out, total, copied, progress, cancelled);
            }
        }
    }

    private void copyFile(Entry entry, File out, long total, long[] copied, Progress progress,
                          AtomicBoolean cancelled) throws IOException {
        Uri documentUri = DocumentsContract.buildDocumentUriUsingTree(treeUri, entry.documentId);
        byte[] buffer = new byte[1 << 20];
        long lastReport = copied[0];
        try (InputStream in = resolver.openInputStream(documentUri);
             OutputStream os = new FileOutputStream(out)) {
            if (in == null) {
                throw new IOException("Cannot read " + entry.name);
            }
            int read;
            while ((read = in.read(buffer)) > 0) {
                if (cancelled.get()) {
                    return;
                }
                os.write(buffer, 0, read);
                copied[0] += read;
                if (copied[0] - lastReport >= (8L << 20)) {
                    lastReport = copied[0];
                    progress.onProgress(copied[0], total, entry.name);
                }
            }
        }
        progress.onProgress(copied[0], total, entry.name);
    }

    private List<Entry> list(String directoryId) throws IOException {
        Uri childrenUri = DocumentsContract.buildChildDocumentsUriUsingTree(treeUri, directoryId);
        List<Entry> entries = new ArrayList<>();
        try (Cursor cursor = resolver.query(childrenUri, COLUMNS, null, null, null)) {
            if (cursor == null) {
                throw new IOException("Cannot list the selected folder");
            }
            while (cursor.moveToNext()) {
                String mime = cursor.getString(2);
                boolean directory = DocumentsContract.Document.MIME_TYPE_DIR.equals(mime);
                long size = cursor.isNull(3) ? -1 : cursor.getLong(3);
                entries.add(new Entry(cursor.getString(0), cursor.getString(1), directory, size));
            }
        }
        return entries;
    }

    /** Document names come from another app; never let one escape the destination folder. */
    private static String sanitize(String name) throws IOException {
        if (name == null || name.isEmpty() || name.equals(".") || name.equals("..")
                || name.indexOf('/') >= 0 || name.indexOf('\0') >= 0) {
            throw new IOException("Unsafe file name in game folder: " + name);
        }
        return name;
    }

    static void deleteRecursively(File file) {
        if (file == null || !file.exists()) {
            return;
        }
        File[] children = file.listFiles();
        if (children != null) {
            for (File child : children) {
                deleteRecursively(child);
            }
        }
        //noinspection ResultOfMethodCallIgnored
        file.delete();
    }
}
