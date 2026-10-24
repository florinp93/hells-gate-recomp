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
        if (mLayout != null) {
            touchControls = new TouchControlsView(this);
            mLayout.addView(touchControls, new android.widget.RelativeLayout.LayoutParams(
                    android.view.ViewGroup.LayoutParams.MATCH_PARENT,
                    android.view.ViewGroup.LayoutParams.MATCH_PARENT));
            // Register the virtual pad once SDL is running so the game sees it from the start.
            touchControls.postDelayed(TouchControlsView::registerGamepad, 3000);
        }
    }

    private TouchControlsView touchControls;

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
                // Diagnostic that scans every draw's indices on the CPU and logs hundreds of
                // warnings per second in this game; far too costly on a phone.
                "--vfetch_oob_trace=false",
                // One Vulkan submission per frame instead of per PM4 primary buffer: each
                // submission costs a large driver allocation on Adreno.
                "--vulkan_submit_on_primary_buffer_end=false",
                // Adreno allocates and zeroes a large block in every vkCmdBeginRendering;
                // classic cached render passes avoid that per-pass cost.
                "--vulkan_dynamic_rendering=false",
                "--show_fps_overlay=" + GamePaths.prefs(this).getBoolean("show_fps", false),
        };
    }
}
