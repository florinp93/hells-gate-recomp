package dev.hellsgate.android;

import android.content.Context;
import android.net.Uri;

import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.HashMap;
import java.util.Map;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * A custom Vulkan driver (for example Mesa Turnip) loaded with libadrenotools in place of the
 * system Adreno driver. One driver is kept, in internal storage: the native linker only loads
 * libraries from there, not from shared storage.
 */
final class GpuDriver {
    private GpuDriver() {}

    private static File folder(Context context) {
        return new File(context.getFilesDir(), "gpu_driver");
    }

    /** The imported driver library, or null when using the system driver. */
    static File library(Context context) {
        String name = GamePaths.prefs(context).getString("gpu_driver_library", "");
        if (name.isEmpty()) return null;
        File file = new File(folder(context), name);
        return file.isFile() ? file : null;
    }

    /** Label for the launcher: the driver's name and version, or "System driver". */
    static String describe(Context context) {
        if (library(context) == null) return "System driver";
        return GamePaths.prefs(context).getString("gpu_driver_name", "Custom driver");
    }

    static void useSystem(Context context) {
        GamePaths.prefs(context).edit().remove("gpu_driver_library").remove("gpu_driver_name")
                .apply();
        deleteTree(folder(context));
    }

    /**
     * Imports a driver package: a .zip with meta.json ({"name", "driverVersion", "libraryName"})
     * as published for Android emulators, a .zip holding one Vulkan .so, or a bare .so.
     * Returns a message for the launcher.
     */
    static String importFrom(Context context, Uri uri) throws IOException {
        Map<String, byte[]> files = new HashMap<>();
        try (InputStream raw = context.getContentResolver().openInputStream(uri)) {
            if (raw == null) throw new IOException("cannot open the file");
            byte[] head = new byte[4];
            java.io.BufferedInputStream in = new java.io.BufferedInputStream(raw);
            in.mark(8);
            int n = in.read(head);
            in.reset();
            boolean zip = n == 4 && head[0] == 'P' && head[1] == 'K';
            boolean elf = n == 4 && head[0] == 0x7F && head[1] == 'E' && head[2] == 'L'
                    && head[3] == 'F';
            if (elf) {
                files.put("vulkan.custom.so", readAll(in, 256 << 20));
            } else if (zip) {
                try (ZipInputStream z = new ZipInputStream(in)) {
                    ZipEntry e;
                    while ((e = z.getNextEntry()) != null) {
                        if (e.isDirectory()) continue;
                        String name = new File(e.getName()).getName();
                        if (name.endsWith(".so") || name.equals("meta.json")) {
                            files.put(name, readAll(z, 256 << 20));
                        }
                    }
                }
            } else {
                throw new IOException("not a .zip or .so file");
            }
        }

        String library = null;
        String label = null;
        byte[] meta = files.get("meta.json");
        if (meta != null) {
            try {
                JSONObject json = new JSONObject(new String(meta, "UTF-8"));
                library = json.optString("libraryName", null);
                String name = json.optString("name", "");
                String version = json.optString("driverVersion", "");
                label = (name + " " + version).trim();
            } catch (org.json.JSONException ignored) {
                // Fall back to finding the library by name below.
            }
        }
        if (library == null || !files.containsKey(library)) {
            library = null;
            for (String name : files.keySet()) {
                if (!name.endsWith(".so")) continue;
                if (library == null || name.contains("vulkan")) library = name;
            }
        }
        if (library == null) throw new IOException("no driver library (.so) in the file");
        if (label == null || label.isEmpty()) label = library;

        File dir = folder(context);
        deleteTree(dir);
        if (!dir.mkdirs()) throw new IOException("cannot create " + dir);
        for (Map.Entry<String, byte[]> f : files.entrySet()) {
            if (!f.getKey().endsWith(".so")) continue;
            try (OutputStream out = new FileOutputStream(new File(dir, f.getKey()))) {
                out.write(f.getValue());
            }
        }
        GamePaths.prefs(context).edit().putString("gpu_driver_library", library)
                .putString("gpu_driver_name", label).apply();
        return "GPU driver set to " + label + ". It is used the next time the game starts.";
    }

    private static byte[] readAll(InputStream in, int limit) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] buf = new byte[1 << 16];
        int n;
        while ((n = in.read(buf)) > 0) {
            out.write(buf, 0, n);
            if (out.size() > limit) throw new IOException("file too large");
        }
        return out.toByteArray();
    }

    private static void deleteTree(File f) {
        File[] children = f.listFiles();
        if (children != null) for (File c : children) deleteTree(c);
        f.delete();
    }
}
