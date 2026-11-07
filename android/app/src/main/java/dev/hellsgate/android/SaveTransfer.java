package dev.hellsgate.android;

import android.content.ContentResolver;
import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.provider.DocumentsContract;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.List;
import java.util.Locale;

/**
 * Imports and exports Dante's Inferno saves. Saves live in the runtime's user data folder as
 * {@code <profile XUID>/454108CF/00000001/<save>/...}, the same layout the PC build of this
 * project and Xenia use, so their save folders can be copied in as they are.
 */
final class SaveTransfer {
    static final String TITLE_ID = "454108CF";
    /** The runtime's built-in offline profile. */
    static final String DEFAULT_XUID = "B13EBABEBABEBABE";

    private static final String[] COLUMNS = {
            DocumentsContract.Document.COLUMN_DOCUMENT_ID,
            DocumentsContract.Document.COLUMN_DISPLAY_NAME,
            DocumentsContract.Document.COLUMN_MIME_TYPE,
    };
    private static final int MAX_SEARCH_DEPTH = 6;

    private final ContentResolver resolver;
    private final Uri treeUri;
    private int files;

    private SaveTransfer(Context context, Uri treeUri) {
        this.resolver = context.getContentResolver();
        this.treeUri = treeUri;
    }

    /**
     * Copies the save data found in the picked folder into this device's profile. The picked
     * folder may be anything that contains a {@code 454108CF} folder (a Xenia "content" folder, the
     * PC build's userdata folder, a profile folder, the 454108CF folder itself), or a folder that
     * directly holds the {@code 00000001} saves folder. Existing saves are backed up first.
     * Returns a short summary.
     */
    static String importSaves(Context context, Uri treeUri) throws IOException {
        return new SaveTransfer(context, treeUri).runImport(context);
    }

    /** Copies this device's saves into a new folder inside the picked folder. */
    static String exportSaves(Context context, Uri treeUri) throws IOException {
        return new SaveTransfer(context, treeUri).runExport(context);
    }

    private String runImport(Context context) throws IOException {
        String rootId = DocumentsContract.getTreeDocumentId(treeUri);
        String rootName = displayName(rootId);
        String titleDirId;
        if (TITLE_ID.equalsIgnoreCase(rootName) || hasChildDir(rootId, "00000001")) {
            titleDirId = rootId;
        } else {
            titleDirId = findDir(rootId, TITLE_ID, 0);
        }
        if (titleDirId == null) {
            throw new IOException("No Dante's Inferno saves found. Pick a folder that contains a "
                    + "\"" + TITLE_ID + "\" folder (for example Xenia's \"content\" folder or the "
                    + "PC version's userdata folder). Saves copied straight off an Xbox 360 "
                    + "(single STFS files) need to be extracted first.");
        }

        File userData = GamePaths.userData(context);
        // Use the profile this device already saves under, else the runtime's default profile.
        String xuid = DEFAULT_XUID;
        File[] profiles = userData.listFiles();
        if (profiles != null) {
            for (File profile : profiles) {
                if (profile.getName().matches("[0-9A-Fa-f]{16}")
                        && new File(profile, TITLE_ID).isDirectory()) {
                    xuid = profile.getName();
                    break;
                }
            }
        }
        File dest = new File(new File(userData, xuid), TITLE_ID);
        if (dest.exists()) {
            String stamp = new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(new Date());
            File backup = new File(new File(userData, "save-backups"), stamp);
            if (!backup.mkdirs() || !dest.renameTo(new File(backup, TITLE_ID))) {
                throw new IOException("Could not back up the existing saves");
            }
        }
        File staging = new File(userData, TITLE_ID + ".importing");
        GameImporter.deleteRecursively(staging);
        if (!staging.mkdirs()) throw new IOException("Cannot create " + staging);
        try {
            copyIn(titleDirId, staging);
        } catch (IOException | RuntimeException e) {
            GameImporter.deleteRecursively(staging);
            throw e;
        }
        File parent = dest.getParentFile();
        if (parent != null) parent.mkdirs();
        if (!staging.renameTo(dest)) {
            throw new IOException("Cannot move imported saves into place");
        }
        return "Imported " + files + " save file(s).";
    }

    private String runExport(Context context) throws IOException {
        File userData = GamePaths.userData(context);
        List<File> titleDirs = new ArrayList<>();
        File[] profiles = userData.listFiles();
        if (profiles != null) {
            for (File profile : profiles) {
                if (profile.getName().equals("save-backups")) continue;
                File t = new File(profile, TITLE_ID);
                if (profile.isDirectory() && t.isDirectory()) titleDirs.add(t);
            }
        }
        if (titleDirs.isEmpty()) {
            throw new IOException("There are no saves to export yet.");
        }
        String stamp = new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(new Date());
        Uri rootDoc = DocumentsContract.buildDocumentUriUsingTree(treeUri,
                DocumentsContract.getTreeDocumentId(treeUri));
        Uri out = createDir(rootDoc, "HellsGate-saves-" + stamp);
        for (File t : titleDirs) {
            Uri profileDir = createDir(out, t.getParentFile().getName());
            copyOut(t, createDir(profileDir, TITLE_ID));
        }
        return "Exported " + files + " save file(s) to HellsGate-saves-" + stamp + ".";
    }

    // --- import helpers ---------------------------------------------------------------------

    private String findDir(String dirId, String name, int depth) throws IOException {
        List<String[]> children = list(dirId);
        for (String[] c : children) {
            if (isDir(c) && name.equalsIgnoreCase(c[1])) return c[0];
        }
        if (depth >= MAX_SEARCH_DEPTH) return null;
        for (String[] c : children) {
            if (isDir(c)) {
                String found = findDir(c[0], name, depth + 1);
                if (found != null) return found;
            }
        }
        return null;
    }

    private boolean hasChildDir(String dirId, String name) throws IOException {
        for (String[] c : list(dirId)) {
            if (isDir(c) && name.equalsIgnoreCase(c[1])) return true;
        }
        return false;
    }

    private String displayName(String docId) {
        Uri uri = DocumentsContract.buildDocumentUriUsingTree(treeUri, docId);
        try (Cursor c = resolver.query(uri, new String[] {
                DocumentsContract.Document.COLUMN_DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst()) return c.getString(0);
        } catch (RuntimeException ignored) {
        }
        return "";
    }

    private void copyIn(String dirId, File target) throws IOException {
        for (String[] c : list(dirId)) {
            File out = new File(target, sanitize(c[1]));
            if (isDir(c)) {
                if (!out.isDirectory() && !out.mkdirs()) throw new IOException("Cannot create " + out);
                copyIn(c[0], out);
            } else {
                Uri uri = DocumentsContract.buildDocumentUriUsingTree(treeUri, c[0]);
                try (InputStream in = resolver.openInputStream(uri);
                     OutputStream os = new FileOutputStream(out)) {
                    if (in == null) throw new IOException("Cannot read " + c[1]);
                    pipe(in, os);
                }
                files++;
            }
        }
    }

    private List<String[]> list(String dirId) throws IOException {
        Uri childrenUri = DocumentsContract.buildChildDocumentsUriUsingTree(treeUri, dirId);
        List<String[]> out = new ArrayList<>();
        try (Cursor cursor = resolver.query(childrenUri, COLUMNS, null, null, null)) {
            if (cursor == null) throw new IOException("Cannot list the selected folder");
            while (cursor.moveToNext()) {
                out.add(new String[] {cursor.getString(0), cursor.getString(1), cursor.getString(2)});
            }
        }
        return out;
    }

    private static boolean isDir(String[] c) {
        return DocumentsContract.Document.MIME_TYPE_DIR.equals(c[2]);
    }

    // --- export helpers ---------------------------------------------------------------------

    private Uri createDir(Uri parent, String name) throws IOException {
        Uri uri = DocumentsContract.createDocument(resolver, parent,
                DocumentsContract.Document.MIME_TYPE_DIR, name);
        if (uri == null) throw new IOException("Cannot create folder " + name);
        return uri;
    }

    private void copyOut(File dir, Uri target) throws IOException {
        File[] children = dir.listFiles();
        if (children == null) return;
        for (File f : children) {
            if (f.isDirectory()) {
                copyOut(f, createDir(target, f.getName()));
            } else {
                Uri doc = DocumentsContract.createDocument(resolver, target,
                        "application/octet-stream", f.getName());
                if (doc == null) throw new IOException("Cannot create " + f.getName());
                try (InputStream in = new FileInputStream(f);
                     OutputStream os = resolver.openOutputStream(doc)) {
                    if (os == null) throw new IOException("Cannot write " + f.getName());
                    pipe(in, os);
                }
                files++;
            }
        }
    }

    private static void pipe(InputStream in, OutputStream os) throws IOException {
        byte[] buf = new byte[1 << 16];
        int n;
        while ((n = in.read(buf)) > 0) os.write(buf, 0, n);
    }

    private static String sanitize(String name) throws IOException {
        if (name == null || name.isEmpty() || name.equals(".") || name.equals("..")
                || name.indexOf('/') >= 0 || name.indexOf('\0') >= 0) {
            throw new IOException("Unsafe file name in save folder: " + name);
        }
        return name;
    }
}
