package dev.hellsgate.android;

import android.content.Context;

import java.io.File;

/** Where the imported game files, saves and shader cache live (app-specific storage). */
final class GamePaths {
    private GamePaths() {}

    static File root(Context context) {
        File external = context.getExternalFilesDir(null);
        return external != null ? external : context.getFilesDir();
    }

    static File game(Context context) {
        return new File(root(context), "game");
    }

    static File userData(Context context) {
        return new File(root(context), "userdata");
    }

    static File cache(Context context) {
        return new File(root(context), "cache");
    }

    static boolean isGameImported(Context context) {
        return new File(game(context), "default.xex").isFile();
    }
}
