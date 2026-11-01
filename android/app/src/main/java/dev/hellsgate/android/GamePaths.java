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

    static android.content.SharedPreferences prefs(Context context) {
        return context.getSharedPreferences("settings", Context.MODE_PRIVATE);
    }

    static final String UPSCALER_OFF = "Bilinear";
    static final String[] UPSCALER_OPTIONS = {UPSCALER_OFF, "SGSR", "SGSR 2"};

    /** The presenter's --present_effect value for the "upscaler" setting. */
    static String presentEffect(Context context) {
        switch (prefs(context).getString("upscaler", UPSCALER_OFF)) {
            case "SGSR": return "sgsr";
            case "SGSR 2": return "sgsr2";
            default: return "bilinear";
        }
    }

    static final String SHARPNESS_DEFAULT = "Normal";
    static final String[] SHARPNESS_OPTIONS = {"Soft", SHARPNESS_DEFAULT, "Sharp"};

    /** The presenter's --present_sgsr_edge_sharpness value (SGSR 1 only). */
    static String sgsrSharpness(Context context) {
        switch (prefs(context).getString("sgsr_sharpness", SHARPNESS_DEFAULT)) {
            case "Soft": return "1.5";
            case "Sharp": return "3.0";
            default: return "2.0";
        }
    }

    static final String QUALITY_DEFAULT = "Quality";
    static final String[] QUALITY_OPTIONS = {"Performance", "Balanced", QUALITY_DEFAULT,
            "Ultra Quality"};

    /**
     * The game's draw size ("--render_resolution") for the "render_quality" setting, as a share
     * of a 1080p output: Performance 50% (960x540), Balanced 59% (1133x637), Quality 67% (the
     * game's own 1280x720). The upscaler fills the screen from it. Never empty: an empty
     * "--name=" argument swallows the argument after it.
     */
    static String renderResolution(Context context) {
        switch (prefs(context).getString("render_quality", QUALITY_DEFAULT)) {
            case "Performance": return "960x540";
            case "Balanced": return "1133x637";
            default: return "1280x720";
        }
    }

    /**
     * Draw resolution scale {x, y}. Ultra Quality draws twice as wide (2560x720): the game can't
     * draw larger than 1280x720 itself (its 10 MB EDRAM overflows), but the emulator can scale it
     * by whole steps.
     */
    static int[] renderScale(Context context) {
        return "Ultra Quality".equals(prefs(context).getString("render_quality", QUALITY_DEFAULT))
                ? new int[] {2, 1} : new int[] {1, 1};
    }

    static final String ASPECT_AUTO = "Auto";
    static final String[] ASPECT_OPTIONS = {ASPECT_AUTO, "16:9", "20:9", "21:9"};

    /**
     * The game's target aspect for the "aspect" setting: 0 keeps the original 16:9 picture,
     * anything wider widens the view. Auto follows the screen.
     */
    static double targetAspect(Context context) {
        String option = prefs(context).getString("aspect", ASPECT_AUTO);
        double aspect;
        switch (option) {
            case "16:9": return 0.0;
            case "20:9": aspect = 20.0 / 9.0; break;
            case "21:9": aspect = 21.0 / 9.0; break;
            default: {
                android.util.DisplayMetrics metrics = new android.util.DisplayMetrics();
                ((android.view.WindowManager) context.getSystemService(Context.WINDOW_SERVICE))
                        .getDefaultDisplay().getRealMetrics(metrics);
                int longSide = Math.max(metrics.widthPixels, metrics.heightPixels);
                int shortSide = Math.min(metrics.widthPixels, metrics.heightPixels);
                aspect = shortSide > 0 ? (double) longSide / shortSide : 0.0;
            }
        }
        // At or below 16:9 the original picture already fits.
        return aspect > 16.0 / 9.0 + 0.01 ? aspect : 0.0;
    }

    static boolean isGameImported(Context context) {
        return new File(game(context), "default.xex").isFile();
    }
}
