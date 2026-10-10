using System;
using System.Collections.Generic;
using System.Globalization;
using System.Runtime.InteropServices;

namespace DantesInferno
{
    public class DisplayAspect
    {
        public string Name { get; set; }
        public double Value { get; set; }
        public int DetectedWidth { get; set; }
        public int DetectedHeight { get; set; }
        public int RefreshRate { get; set; }
    }

    public class AspectOption
    {
        public string Label { get; set; }
        public string Key { get; set; }
        public double Value { get; set; }
    }

    public class FrameRateOption
    {
        public string Label { get; set; }
        public int Value { get; set; }
    }

    public static class DisplayOptions
    {
        public const string AspectAuto = "auto";
        public const int DefaultFrameRate = 120;
        public const int MinResolution = 320;
        public const int MaxResolution = 16384;

        // Multiples of 60: the game's UI updates at 60 Hz.
        public static readonly int[] FrameRates = { 60, 120, 180, 240 };

        public static readonly List<AspectOption> AspectOptions = new List<AspectOption>
        {
            new AspectOption { Label = "Auto (match resolution)", Key = AspectAuto, Value = 0 },
            new AspectOption { Label = "4:3", Key = "4:3", Value = 4.0 / 3.0 },
            new AspectOption { Label = "16:9", Key = "16:9", Value = 16.0 / 9.0 },
            new AspectOption { Label = "21:9", Key = "21:9", Value = 64.0 / 27.0 },
            new AspectOption { Label = "32:9", Key = "32:9", Value = 32.0 / 9.0 },
        };

        private static readonly int[,] CommonResolutions =
        {
            { 1280, 720 }, { 1600, 900 }, { 1920, 1080 }, { 2560, 1440 }, { 3840, 2160 },
            { 1440, 1080 }, { 1600, 1200 }, { 1920, 1440 },
            { 1920, 1200 }, { 2560, 1600 },
            { 2560, 1080 }, { 3440, 1440 }, { 5120, 2160 },
            { 3840, 1080 }, { 5120, 1440 },
        };

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        private struct DEVMODE
        {
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmDeviceName;
            public short dmSpecVersion;
            public short dmDriverVersion;
            public short dmSize;
            public short dmDriverExtra;
            public int dmFields;
            public int dmPositionX;
            public int dmPositionY;
            public int dmDisplayOrientation;
            public int dmDisplayFixedOutput;
            public short dmColor;
            public short dmDuplex;
            public short dmYResolution;
            public short dmTTOption;
            public short dmCollate;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmFormName;
            public short dmLogPixels;
            public int dmBitsPerPel;
            public int dmPelsWidth;
            public int dmPelsHeight;
            public int dmDisplayFlags;
            public int dmDisplayFrequency;
            public int dmICMMethod;
            public int dmICMIntent;
            public int dmMediaType;
            public int dmDitherType;
            public int dmReserved1;
            public int dmReserved2;
            public int dmPanningWidth;
            public int dmPanningHeight;
        }

        private const int ENUM_CURRENT_SETTINGS = -1;

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern bool EnumDisplaySettings(string deviceName, int modeNum, ref DEVMODE devMode);

        [DllImport("user32.dll")]
        private static extern int GetSystemMetrics(int nIndex);

        // Primary display mode in physical pixels, and its refresh rate.
        public static DisplayAspect DetectPrimaryAspect()
        {
            int width = 0;
            int height = 0;
            int refresh = 0;
            try
            {
                var mode = new DEVMODE();
                mode.dmSize = (short)Marshal.SizeOf(typeof(DEVMODE));
                if (EnumDisplaySettings(null, ENUM_CURRENT_SETTINGS, ref mode))
                {
                    width = mode.dmPelsWidth;
                    height = mode.dmPelsHeight;
                    refresh = mode.dmDisplayFrequency;
                }
                if (width <= 0 || height <= 0)
                {
                    width = GetSystemMetrics(0);
                    height = GetSystemMetrics(1);
                }
            }
            catch
            {
                width = 0;
                height = 0;
            }

            if (width <= 0 || height <= 0)
            {
                width = 1920;
                height = 1080;
            }
            // 0 and 1 mean the hardware default.
            if (refresh <= 1)
                refresh = 60;

            return new DisplayAspect
            {
                Name = AspectName((double)width / height),
                Value = (double)width / height,
                DetectedWidth = width,
                DetectedHeight = height,
                RefreshRate = refresh,
            };
        }

        public static string AspectName(double aspect)
        {
            var named = new[]
            {
                new KeyValuePair<string, double>("5:4", 1.25),
                new KeyValuePair<string, double>("4:3", 4.0 / 3.0),
                new KeyValuePair<string, double>("16:10", 1.6),
                new KeyValuePair<string, double>("16:9", 16.0 / 9.0),
                new KeyValuePair<string, double>("21:9", 2.3889),
                new KeyValuePair<string, double>("32:9", 32.0 / 9.0),
            };
            foreach (var n in named)
            {
                if (Math.Abs(aspect - n.Value) <= 0.03)
                    return n.Key;
            }
            return aspect.ToString("0.##", CultureInfo.InvariantCulture) + ":1";
        }

        public static string FormatResolution(int width, int height)
        {
            return string.Format(CultureInfo.InvariantCulture, "{0}x{1}", width, height);
        }

        public static string NativeResolution(DisplayAspect display)
        {
            return FormatResolution(display.DetectedWidth, display.DetectedHeight);
        }

        // "WIDTHxHEIGHT" (x, X, * or ×; spaces allowed).
        public static bool TryParseResolution(string text, out int width, out int height)
        {
            width = 0;
            height = 0;
            if (string.IsNullOrWhiteSpace(text))
                return false;
            var parts = text.Replace(" ", "").Split('x', 'X', '*', '×');
            if (parts.Length != 2)
                return false;
            if (!int.TryParse(parts[0], NumberStyles.None, CultureInfo.InvariantCulture, out width) ||
                !int.TryParse(parts[1], NumberStyles.None, CultureInfo.InvariantCulture, out height))
                return false;
            return width >= MinResolution && height >= MinResolution &&
                   width <= MaxResolution && height <= MaxResolution;
        }

        // The display's own resolution first, then common ones.
        public static List<string> BuildResolutionOptions(DisplayAspect display)
        {
            var options = new List<string>();
            options.Add(NativeResolution(display));
            for (int i = 0; i < CommonResolutions.GetLength(0); i++)
            {
                string r = FormatResolution(CommonResolutions[i, 0], CommonResolutions[i, 1]);
                if (!options.Contains(r))
                    options.Add(r);
            }
            return options;
        }

        // Highest frame rate the display shows: above its refresh rate the
        // driver can hold presents to the refresh and slow the game down.
        // 59.94 Hz modes report 59.
        public static int MaxFrameRate(DisplayAspect display)
        {
            int refresh = display != null ? display.RefreshRate : 60;
            int max = FrameRates[0];
            foreach (int r in FrameRates)
            {
                if (r <= refresh + 1)
                    max = r;
            }
            return max;
        }

        public static List<FrameRateOption> BuildFrameRateOptions(DisplayAspect display)
        {
            int max = MaxFrameRate(display);
            var options = new List<FrameRateOption>();
            foreach (int rate in FrameRates)
            {
                if (rate <= max)
                    options.Add(new FrameRateOption { Label = rate + " FPS", Value = rate });
            }
            return options;
        }

        // The nearest supported frame rate.
        public static int NormalizeFrameRate(int rate)
        {
            int best = DefaultFrameRate;
            int bestDistance = int.MaxValue;
            foreach (int r in FrameRates)
            {
                int distance = Math.Abs(r - rate);
                if (distance < bestDistance)
                {
                    best = r;
                    bestDistance = distance;
                }
            }
            return best;
        }

        // The configured frame rate, limited to what the display shows.
        public static int FrameRateFor(GameConfig config, DisplayAspect display)
        {
            return Math.Min(NormalizeFrameRate(config.FrameRate), MaxFrameRate(display));
        }

        public static string NormalizeAspect(string key)
        {
            foreach (var option in AspectOptions)
            {
                if (option.Key.Equals(key ?? "", StringComparison.OrdinalIgnoreCase))
                    return option.Key;
            }
            return AspectAuto;
        }

        public static bool IsRendererFallback(GameConfig config)
        {
            return config.RendererOverride.Equals("xenos", StringComparison.OrdinalIgnoreCase);
        }

        public static string BuildLaunchArguments(GameConfig config, string gameDataRoot, DisplayAspect display)
        {
            if (config == null)
                throw new ArgumentNullException("config");
            if (display == null)
                display = DetectPrimaryAspect();

            var args = new List<string>();
            args.Add(string.Format("--game_data_root=\"{0}\"", gameDataRoot));

            if (IsRendererFallback(config))
            {
                args.Add("--renderer=xenos");
                args.Add("--render_target_path_d3d12=rov");
                args.Add("--native_render_scale=auto");
            }
            else
            {
                args.Add("--renderer=dante");
                int width, height;
                string resolution = TryParseResolution(config.RenderResolution, out width, out height)
                    ? FormatResolution(width, height)
                    : NativeResolution(display);
                args.Add("--dante_resolution=" + resolution);
            }

            string aspect = NormalizeAspect(config.Aspect);
            foreach (var option in AspectOptions)
            {
                if (option.Key == aspect && option.Value > 0)
                    args.Add(string.Format(CultureInfo.InvariantCulture,
                        "--ultrawide_target_aspect={0:0.####}", option.Value));
            }

            // The game paces to its vblank, which follows the video mode's
            // refresh rate; vsync keeps that vblank running at that rate.
            args.Add(string.Format(CultureInfo.InvariantCulture,
                "--video_mode_refresh_rate={0}", FrameRateFor(config, display)));
            args.Add("--vsync=true");

            if (config.ShowFpsOverlay)
                args.Add("--show_fps_overlay=true");

            if (config.AnisotropicOverride >= 0)
                args.Add(string.Format(CultureInfo.InvariantCulture, "--anisotropic_override={0}", config.AnisotropicOverride));

            args.Add(string.Format("--fullscreen={0}", config.Fullscreen.ToString().ToLowerInvariant()));

            args.Add("--input_backend=" + config.InputBackend);

            // Logs are always written, flushed every 2 s so a freeze or a
            // killed process still leaves them complete.
            args.Add(config.DetailedLogging ? "--log_level=debug" : "--log_level=info");
            args.Add("--log_flush_interval=2");

            if (config.GlyphFamily.Equals("playstation", StringComparison.OrdinalIgnoreCase))
                args.Add("--glyph_family=playstation");

            if (config.UserLanguage != 1)
                args.Add(string.Format(CultureInfo.InvariantCulture, "--user_language={0}", config.UserLanguage));

            var keybinds = new Dictionary<string, string>
            {
                { "keybind_a", config.KeybindA },
                { "keybind_b", config.KeybindB },
                { "keybind_x", config.KeybindX },
                { "keybind_y", config.KeybindY },
                { "keybind_left_shoulder", config.KeybindLeftShoulder },
                { "keybind_right_shoulder", config.KeybindRightShoulder },
                { "keybind_left_trigger", config.KeybindLeftTrigger },
                { "keybind_right_trigger", config.KeybindRightTrigger },
                { "keybind_lstick_up", config.KeybindLStickUp },
                { "keybind_lstick_down", config.KeybindLStickDown },
                { "keybind_lstick_left", config.KeybindLStickLeft },
                { "keybind_lstick_right", config.KeybindLStickRight },
                { "keybind_lstick_press", config.KeybindLStickPress },
                { "keybind_rstick_up", config.KeybindRStickUp },
                { "keybind_rstick_down", config.KeybindRStickDown },
                { "keybind_rstick_left", config.KeybindRStickLeft },
                { "keybind_rstick_right", config.KeybindRStickRight },
                { "keybind_rstick_press", config.KeybindRStickPress },
                { "keybind_dpad_up", config.KeybindDpadUp },
                { "keybind_dpad_down", config.KeybindDpadDown },
                { "keybind_dpad_left", config.KeybindDpadLeft },
                { "keybind_dpad_right", config.KeybindDpadRight },
                { "keybind_back", config.KeybindBack },
                { "keybind_start", config.KeybindStart },
            };
            foreach (var kv in keybinds)
            {
                if (!string.IsNullOrEmpty(kv.Value))
                    args.Add(string.Format("--{0}={1}", kv.Key, kv.Value));
            }

            return string.Join(" ", args);
        }
    }
}
