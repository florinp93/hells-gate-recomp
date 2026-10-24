package dev.hellsgate.android;

import android.content.Intent;
import android.os.Bundle;

import org.libsdl.app.SDLActivity;

/**
 * Hosts the recompiled game. SDLActivity loads libSDL3.so and libmain.so and calls the exported
 * SDL_main, which runs the ReXGlue windowed app with the arguments below.
 */
public class HellsGateActivity extends SDLActivity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        if (!GamePaths.isGameImported(this)) {
            // Nothing to run yet: go back to the launcher instead of starting the runtime.
            startActivity(new Intent(this, LauncherActivity.class));
            finish();
            super.onCreate(savedInstanceState);
            return;
        }
        super.onCreate(savedInstanceState);
    }

    // SDL requests "full user" rotation for a resizable window, which overrides the manifest and
    // let the game window turn portrait. Keep it landscape.
    @Override
    public void setOrientationBis(int w, int h, boolean resizable, String hint) {
        setRequestedOrientation(android.content.pm.ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
    }

    @Override
    protected String[] getArguments() {
        GamePaths.userData(this).mkdirs();
        GamePaths.cache(this).mkdirs();
        // The SDK defaults to <executable folder>/logs, which is the read-only native lib dir.
        java.io.File logs = new java.io.File(GamePaths.root(this), "logs");
        logs.mkdirs();
        return new String[] {
                "--game_data_root=" + GamePaths.game(this).getAbsolutePath(),
                "--user_data_root=" + GamePaths.userData(this).getAbsolutePath(),
                "--cache_root=" + GamePaths.cache(this).getAbsolutePath(),
                "--log_file=" + new java.io.File(logs, "dantes_inferno.log").getAbsolutePath(),
                // Empty (the default) disables GPU emulation entirely.
                "--gpu_plugin=xenos",
        };
    }
}
