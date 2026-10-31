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
                // Sparse binding is unreliable on Adreno drivers; allocate the 512 MB guest
                // shared-memory buffer up front instead of binding pages as levels stream in.
                "--vulkan_sparse_shared_memory=false",
                "--protection_shadow=" + GamePaths.prefs(this).getBoolean("protection_shadow", false),
                // Deeper audio queue (32 x 256-sample frames, ~170 ms): the default 8 underruns on
                // phones and the gaps are heard as crackling.
                "--audio_maxqframes=32",
                // The game simulates at the guest video refresh rate: 30 gives a true 30 fps mode.
                "--video_mode_refresh_rate="
                        + (GamePaths.prefs(this).getBoolean("lock_30fps", false) ? "30" : "60"),
                // Classic render passes avoid a large per-vkCmdBeginRendering driver allocation
                // on Adreno (faster), but are experimental here: optional, off by default.
                "--vulkan_dynamic_rendering="
                        + !GamePaths.prefs(this).getBoolean("classic_render_passes", false),
                "--show_fps_overlay=" + GamePaths.prefs(this).getBoolean("show_fps", false),
                // Launcher diagnostics option: GPU timestamps around every render pass.
                "--gpu_timing=" + GamePaths.prefs(this).getBoolean("gpu_timing", false),
                // Wider than 16:9 widens the game's view and fills the screen to that shape.
                "--ultrawide_target_aspect="
                        + String.format(java.util.Locale.ROOT, "%.4f", GamePaths.targetAspect(this)),
        };
    }
}
