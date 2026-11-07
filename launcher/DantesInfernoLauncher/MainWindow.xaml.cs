using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace DantesInferno.Launcher
{
    public partial class MainWindow : Window
    {
        private GameConfig _config;
        private string _installDir;
        private bool _gameRunning;
        private DisplayAspect _displayAspect;
        private List<string> _resolutionOptions;
        private string _launcherLanguage = LauncherLocalizer.DefaultLanguage;
        private bool _populatingLauncherLanguage;
        private readonly bool _autoPlay = Environment.GetCommandLineArgs()
            .Skip(1).Any(a => string.Equals(a, "--play", StringComparison.OrdinalIgnoreCase));

        private const string IssuesUrl = "https://github.com/florinp93/hells-gate-recomp/issues";
        // Texture filtering choices; index 0 ("Default") leaves the game's own.
        private static readonly int[] AnisoLevels = { 0, 1, 2, 4, 8, 16 };

        private string Lt(string key) => LauncherLocalizer.Get(_launcherLanguage, key);
        private string Lf(string key, params object[] args) => LauncherLocalizer.Get(_launcherLanguage, key, args);

        public MainWindow()
        {
            InitializeComponent();
            TabControls.AddHandler(System.Windows.Controls.TextBox.PreviewMouseDownEvent,
                new System.Windows.Input.MouseButtonEventHandler(KeyField_PreviewMouseDown), true);
            InitializeTheme();
            LoadConfiguration();
            PopulateControls();
            ApplyLocalization();
            RefreshPlayStatus();
            RefreshDlcStatus();
            if (!_autoPlay)
                CheckForUpdatesOnStartup();
            RefreshSettingsSummary();
            Closing += (s, e) => AutoSave();
            if (_autoPlay)
            {
                WindowState = WindowState.Minimized;
                ShowInTaskbar = false;
                Loaded += (s, e) =>
                {
                    string gameData = _config.GameDataRoot ?? PathHelper.GetGameDataPath(_installDir);
                    if (!File.Exists(Path.Combine(gameData, "default.xex")))
                    {
                        ShowLauncher();
                        return;
                    }
                    PlayButton_Click(null, null);
                    if (!_gameRunning)
                        ShowLauncher();
                };
            }
            ContentRendered += (s, e) =>
            {
                string gameData = _config.GameDataRoot ?? PathHelper.GetGameDataPath(_installDir);
                if (!File.Exists(Path.Combine(gameData, "default.xex")))
                    OfferIsoImport(gameData);
            };
        }

        private void CheckForUpdatesOnStartup()
        {
            Task.Factory.StartNew(new Action(() =>
            {
                ReleaseInfo release = null;
                try { release = GitHubUpdater.CheckLatest(); }
                catch { return; }

                if (release == null)
                    return;

                var localVersion = GitHubUpdater.GetLocalVersion(_installDir);
                if (release.Version <= localVersion)
                    return;

                if (FindInstallerAsset(release) == null)
                    return;

                Dispatcher.Invoke(new Action(() =>
                {
                    _pendingUpdate = release;
                    DownloadUpdateButton.Visibility = Visibility.Visible;
                    UpdateStatusText.Text =
                        $"Update available: {release.TagName} (you have {localVersion})\n\n" +
                        $"{release.Name}\n\nClick \"Download & Install\" to update.";

                    var result = MessageBox.Show(this,
                        $"A new version is available: {release.TagName}\n" +
                        $"You currently have: {localVersion}\n\n" +
                        $"{release.Name}\n\n" +
                        "Would you like to download and install the update now?",
                        "Update Available",
                        MessageBoxButton.YesNo, MessageBoxImage.Question);

                    if (result == MessageBoxResult.Yes)
                    {
                        MainTabControl.SelectedIndex = 2;
                        DownloadUpdate_Click(null, null);
                    }
                }));
            }));
        }

        private void InitializeTheme()
        {
            try
            {
                string iconPath = Path.Combine(_installDir ?? PathHelper.ExecutableDirectory, "icon.ico");
                if (File.Exists(iconPath))
                    this.Icon = BitmapFrame.Create(new Uri(iconPath, UriKind.Absolute));

                string bannerPath = Path.Combine(_installDir ?? PathHelper.ExecutableDirectory, "banner.jpg");
                if (File.Exists(bannerPath))
                    BannerImage.Source = new BitmapImage(new Uri(bannerPath, UriKind.Absolute));
            }
            catch { }
        }

        private void LoadConfiguration()
        {
            _installDir = PathHelper.ExecutableDirectory;
            string configPath = PathHelper.GetGameConfigPath(_installDir);
            _config = GameConfig.Load(configPath);

            // The installer writes an absolute path; after the folder is moved it
            // points at the old place, so the game\ next to the launcher wins.
            string localGameData = PathHelper.GetGameDataPath(_installDir);
            if (string.IsNullOrWhiteSpace(_config.GameDataRoot) ||
                (!File.Exists(Path.Combine(_config.GameDataRoot, "default.xex")) &&
                 File.Exists(Path.Combine(localGameData, "default.xex"))))
                _config.GameDataRoot = localGameData;

            _config.MigrateLegacySettings();
            _displayAspect = DisplayOptions.DetectPrimaryAspect();
        }

        private void PopulateControls()
        {
            if (_displayAspect == null)
                _displayAspect = DisplayOptions.DetectPrimaryAspect();

            DetectedDisplayText.Text = string.Format(CultureInfo.InvariantCulture,
                "{0}x{1} @ {2} Hz detected ({3})",
                _displayAspect.DetectedWidth, _displayAspect.DetectedHeight, _displayAspect.RefreshRate,
                _displayAspect.Name);

            _resolutionOptions = DisplayOptions.BuildResolutionOptions(_displayAspect);
            ResolutionCombo.ItemsSource = _resolutionOptions;
            ResolutionCombo.Text = DisplayOptions.TryParseResolution(_config.RenderResolution, out int width, out int height)
                ? DisplayOptions.FormatResolution(width, height)
                : DisplayOptions.NativeResolution(_displayAspect);

            AspectCombo.ItemsSource = DisplayOptions.AspectOptions;
            AspectCombo.DisplayMemberPath = "Label";
            AspectCombo.SelectedValuePath = "Key";
            AspectCombo.SelectedValue = _config.Aspect;

            FrameRateCombo.ItemsSource = DisplayOptions.BuildFrameRateOptions(
                _displayAspect, Lt(LauncherLocalizer.FrameRateExperimental));
            FrameRateCombo.DisplayMemberPath = "Label";
            FrameRateCombo.SelectedValuePath = "Value";
            FrameRateCombo.SelectedValue = DisplayOptions.FrameRateFor(_config, _displayAspect);

            AnisoCombo.ItemsSource = new List<string> { "Default", "1x", "2x", "4x", "8x", "16x" };
            int aniso = _config.AnisotropicOverride < 0
                ? 0 : Array.IndexOf(AnisoLevels, _config.AnisotropicOverride);
            AnisoCombo.SelectedIndex = Math.Max(aniso, 0);

            PopulateShadowCombo(_config.ShadowScale);

            FullscreenCheck.IsChecked = _config.Fullscreen;
            SmaaCheck.IsChecked = _config.Smaa;
            FpsOverlayCheck.IsChecked = _config.ShowFpsOverlay;

            ControllerFixCheck.IsChecked = _config.InputBackend.Equals("sdl", StringComparison.OrdinalIgnoreCase);

            PopulateGlyphCombo(_config.GlyphFamily);

            LoggingEnabledCheck.IsChecked = _config.DetailedLogging;
            PortableCheck.IsChecked = File.Exists(PathHelper.GetPortableMarkerPath(_installDir));

            PopulateLanguageCombo();
            PopulateLauncherLanguageCombo();
            PopulateKeybinds();
        }

        private void PopulateShadowCombo(int selected)
        {
            var items = new List<KeyValuePair<int, string>>
            {
                new KeyValuePair<int, string>(1, Lt(LauncherLocalizer.ShadowOriginal)),
                new KeyValuePair<int, string>(2, Lt(LauncherLocalizer.ShadowHigh)),
                new KeyValuePair<int, string>(4, Lt(LauncherLocalizer.ShadowUltra)),
            };
            ShadowCombo.ItemsSource = items;
            ShadowCombo.DisplayMemberPath = "Value";
            ShadowCombo.SelectedValuePath = "Key";
            ShadowCombo.SelectedValue = DisplayOptions.NormalizeShadowScale(selected);
        }

        private void PopulateGlyphCombo(string selected)
        {
            var items = new List<KeyValuePair<string, string>>
            {
                new KeyValuePair<string, string>("auto", Lt(LauncherLocalizer.GlyphAuto)),
                new KeyValuePair<string, string>("xbox360", Lt(LauncherLocalizer.GlyphXbox)),
                new KeyValuePair<string, string>("playstation4", Lt(LauncherLocalizer.GlyphPlayStation4)),
                new KeyValuePair<string, string>("playstation5", Lt(LauncherLocalizer.GlyphPlayStation5)),
                new KeyValuePair<string, string>("keyboard", Lt(LauncherLocalizer.GlyphKeyboard)),
            };
            GlyphFamilyCombo.ItemsSource = items;
            GlyphFamilyCombo.DisplayMemberPath = "Value";
            GlyphFamilyCombo.SelectedValuePath = "Key";
            GlyphFamilyCombo.SelectedValue = items.Exists(i => i.Key == selected) ? selected : "auto";
        }

        private void PopulateLauncherLanguageCombo()
        {
            _populatingLauncherLanguage = true;
            var items = new List<KeyValuePair<string, string>>();
            foreach (var lang in LauncherLocalizer.SupportedLanguages)
            {
                string display = LauncherLocalizer.LanguageDisplayNames.ContainsKey(lang)
                    ? LauncherLocalizer.LanguageDisplayNames[lang] : lang;
                items.Add(new KeyValuePair<string, string>(lang, display));
            }
            LauncherLanguageCombo.ItemsSource = items;
            LauncherLanguageCombo.DisplayMemberPath = "Value";
            LauncherLanguageCombo.SelectedValuePath = "Key";

            _launcherLanguage = LauncherLocalizer.NormalizeLanguage(_config.LauncherLanguage);
            int idx = items.FindIndex(i => i.Key == _launcherLanguage);
            if (idx < 0) idx = 0;
            LauncherLanguageCombo.SelectedIndex = idx;
            _populatingLauncherLanguage = false;
        }

        private void LauncherLanguageCombo_SelectionChanged(object sender, System.Windows.Controls.SelectionChangedEventArgs e)
        {
            if (_populatingLauncherLanguage)
                return;
            if (LauncherLanguageCombo.SelectedValue is string lang)
            {
                _launcherLanguage = LauncherLocalizer.NormalizeLanguage(lang);
                _config.LauncherLanguage = _launcherLanguage;
                SaveConfig();
                ApplyLocalization();
            }
        }

        private void ApplyLocalization()
        {
            TabPlay.Header = Lt(LauncherLocalizer.TabPlay);
            TabDlc.Header = Lt(LauncherLocalizer.TabDlc);
            TabControls.Header = Lt(LauncherLocalizer.TabControls);
            TabSettings.Header = Lt(LauncherLocalizer.TabSettings);
            TabUpdates.Header = Lt(LauncherLocalizer.TabUpdates);

            PlaySubtitleText.Text = Lt(LauncherLocalizer.PlaySubtitle);
            PlayButton.Content = Lt(LauncherLocalizer.PlayButton);
            ExitButton.ToolTip = Lt(LauncherLocalizer.ExitButton);
            ChangeSettingsText.Text = Lt(LauncherLocalizer.ChangeSettings);
            DropDlcTitleText.Text = Lt(LauncherLocalizer.DropDlcTitle);
            DropDlcHintText.Text = Lt(LauncherLocalizer.DropDlcHint);
            SupportButton.Content = Lt(LauncherLocalizer.SupportDeveloper);
            var version = GitHubUpdater.GetLocalVersion(_installDir);
            VersionText.Text = Lf(LauncherLocalizer.VersionLabel, version.ToString());

            DlcTitleText.Text = Lt(LauncherLocalizer.DlcTitle);
            DlcDescriptionText.Text = Lt(LauncherLocalizer.DlcDescription);
            OpenDlcFolderButton.Content = Lt(LauncherLocalizer.OpenDlcFolder);
            InstallDlcButton.Content = Lt(LauncherLocalizer.InstallDlc);
            OpenTuFolderButton.Content = Lt(LauncherLocalizer.OpenTuFolder);
            OpenTuFolderButton.ToolTip = Lt(LauncherLocalizer.TuFolderTooltip);

            TrialsServerTitleText.Text = Lt(LauncherLocalizer.TrialsServerTitle);
            TrialsServerMessageText.Text = Lt(LauncherLocalizer.TrialsServerMessage);
            TrialsServerButton.Content = Lt(LauncherLocalizer.TrialsServerButton);

            ControlsTitleText.Text = Lt(LauncherLocalizer.ControlsTitle);
            ControlsHintText.Text = Lt(LauncherLocalizer.ControlsHint);
            GroupActions.Header = Lt(LauncherLocalizer.GroupActions);
            GroupShoulders.Header = Lt(LauncherLocalizer.GroupShoulders);
            GroupLeftStick.Header = Lt(LauncherLocalizer.GroupLeftStick);
            GroupRightStick.Header = Lt(LauncherLocalizer.GroupRightStick);
            GroupDpad.Header = Lt(LauncherLocalizer.GroupDpad);
            LabelA.Content = Lt(LauncherLocalizer.LabelA);
            LabelB.Content = Lt(LauncherLocalizer.LabelB);
            LabelX.Content = Lt(LauncherLocalizer.LabelX);
            LabelY.Content = Lt(LauncherLocalizer.LabelY);
            LabelBack.Content = Lt(LauncherLocalizer.LabelBack);
            LabelStart.Content = Lt(LauncherLocalizer.LabelStart);
            LabelLeftShoulder.Content = Lt(LauncherLocalizer.LabelLeftShoulder);
            LabelRightShoulder.Content = Lt(LauncherLocalizer.LabelRightShoulder);
            LabelLeftTrigger.Content = Lt(LauncherLocalizer.LabelLeftTrigger);
            LabelRightTrigger.Content = Lt(LauncherLocalizer.LabelRightTrigger);
            LabelLUp.Content = Lt(LauncherLocalizer.LabelUp);
            LabelLDown.Content = Lt(LauncherLocalizer.LabelDown);
            LabelLLeft.Content = Lt(LauncherLocalizer.LabelLeft);
            LabelLRight.Content = Lt(LauncherLocalizer.LabelRight);
            LabelLPress.Content = Lt(LauncherLocalizer.LabelPressSprint);
            LabelRUp.Content = Lt(LauncherLocalizer.LabelUp);
            LabelRDown.Content = Lt(LauncherLocalizer.LabelDown);
            LabelRLeft.Content = Lt(LauncherLocalizer.LabelLeft);
            LabelRRight.Content = Lt(LauncherLocalizer.LabelRight);
            LabelRPress.Content = Lt(LauncherLocalizer.LabelPress);
            LabelDUp.Content = Lt(LauncherLocalizer.LabelUp);
            LabelDDown.Content = Lt(LauncherLocalizer.LabelDown);
            LabelDLeft.Content = Lt(LauncherLocalizer.LabelLeft);
            LabelDRight.Content = Lt(LauncherLocalizer.LabelRight);
            BindingsAppliedNoteText.Text = Lt(LauncherLocalizer.BindingsAppliedNote);
            ResetKeybindsButton.Content = Lt(LauncherLocalizer.ResetToDefault);
            SaveKeybindsButton.Content = Lt(LauncherLocalizer.SaveBindings);

            GroupGraphics.Header = Lt(LauncherLocalizer.GroupGraphics);
            LabelDisplay.Content = Lt(LauncherLocalizer.LabelDisplay);
            LabelResolution.Content = Lt(LauncherLocalizer.LabelResolution);
            LabelAspectRatio.Content = Lt(LauncherLocalizer.LabelAspectRatio);
            LabelFrameRate.Content = Lt(LauncherLocalizer.LabelFrameRate);
            LabelTextureFiltering.Content = Lt(LauncherLocalizer.LabelTextureFiltering);
            LabelGameLanguage.Content = Lt(LauncherLocalizer.LabelGameLanguage);
            LabelLauncherLanguage.Content = Lt(LauncherLocalizer.LabelLauncherLanguage);
            FullscreenCheck.Content = Lt(LauncherLocalizer.CheckFullscreen);
            SmaaCheck.Content = Lt(LauncherLocalizer.CheckSmaa);
            SmaaCheck.ToolTip = Lt(LauncherLocalizer.SmaaTooltip);
            LabelShadowQuality.Content = Lt(LauncherLocalizer.LabelShadowQuality);
            ShadowCombo.ToolTip = Lt(LauncherLocalizer.ShadowTooltip);
            PopulateShadowCombo(ShadowCombo.SelectedValue is int shadow ? shadow : _config.ShadowScale);
            GroupControlsSettings.Header = Lt(LauncherLocalizer.GroupControls);
            ControllerFixCheck.Content = Lt(LauncherLocalizer.CheckControllerBackend);
            ControllerFixCheck.ToolTip = Lt(LauncherLocalizer.ControllerTooltip);
            LabelButtonGlyphs.Text = Lt(LauncherLocalizer.LabelButtonGlyphs);
            GlyphFamilyCombo.ToolTip = Lt(LauncherLocalizer.GlyphTooltip);
            PopulateGlyphCombo(GlyphFamilyCombo.SelectedValue as string ?? _config.GlyphFamily);
            LoggingEnabledCheck.Content = Lt(LauncherLocalizer.CheckLogging);
            PortableCheck.Content = Lt(LauncherLocalizer.CheckPortable);
            PortableCheck.ToolTip = Lt(LauncherLocalizer.PortableTooltip);
            SettingsAppliedNoteText.Text = Lt(LauncherLocalizer.SettingsAppliedNote);
            ApplyRecommendedButton.Content = Lt(LauncherLocalizer.ResetToRecommended);
            SaveSettingsButton.Content = Lt(LauncherLocalizer.SaveSettings);
            ResolutionCombo.ToolTip = Lt(LauncherLocalizer.ResolutionTooltip);
            AspectCombo.ToolTip = Lt(LauncherLocalizer.AspectTooltip);
            FrameRateCombo.ToolTip = Lt(LauncherLocalizer.FrameRateTooltip);
            LanguageCombo.ToolTip = Lt(LauncherLocalizer.LanguageGameTooltip);
            LauncherLanguageCombo.ToolTip = Lt(LauncherLocalizer.LanguageLauncherTooltip);

            UpdatesTitleText.Text = Lt(LauncherLocalizer.UpdatesTitle);
            UpdateVersionText.Text = Lf(LauncherLocalizer.InstalledVersion, version.ToString());
            CheckUpdatesButton.Content = Lt(LauncherLocalizer.CheckForUpdates);
            DownloadUpdateButton.Content = Lt(LauncherLocalizer.DownloadInstall);

            RefreshPlayStatus();
            RefreshDlcStatus();
        }

        private void PopulateLanguageCombo()
        {
            var languageItems = new List<KeyValuePair<uint, string>>();
            string gameDir = _config.GameDataRoot ?? PathHelper.GetGameDataPath(_installDir);
            string xexPath = Path.Combine(gameDir, "default.xex");

            // Manifest is authoritative: only languages actually present on the disc.
            var discLangs = LanguageManifest.GetDiscTextLanguages(gameDir);
            if (discLangs != null)
            {
                foreach (var entry in discLangs)
                {
                    string name = XexParser.LanguageNames.TryGetValue(entry.Id, out string known)
                        ? known : entry.Name;
                    languageItems.Add(new KeyValuePair<uint, string>(entry.Id, name));
                }
            }
            else if (File.Exists(xexPath))
            {
                // No manifest: fall back to XEX region flags.
                var xexInfo = XexParser.Parse(xexPath);
                if (xexInfo != null)
                {
                    foreach (uint langId in XexParser.GetSupportedLanguages(xexInfo.Region))
                    {
                        if (XexParser.LanguageNames.TryGetValue(langId, out string name))
                            languageItems.Add(new KeyValuePair<uint, string>(langId, name));
                    }
                }
            }

            if (languageItems.Count == 0)
                languageItems.Add(new KeyValuePair<uint, string>(1, "English"));

            LanguageCombo.ItemsSource = languageItems;
            LanguageCombo.DisplayMemberPath = "Value";
            LanguageCombo.SelectedValuePath = "Key";

            uint current = _config.UserLanguage;
            int idx = languageItems.FindIndex(l => l.Key == current);
            if (idx < 0) idx = 0;
            LanguageCombo.SelectedIndex = idx;
        }

        private void PopulateKeybinds()
        {
            KeyA.Text = _config.KeybindA;
            KeyB.Text = _config.KeybindB;
            KeyX.Text = _config.KeybindX;
            KeyY.Text = _config.KeybindY;
            KeyBack.Text = _config.KeybindBack;
            KeyStart.Text = _config.KeybindStart;
            KeyLS.Text = _config.KeybindLeftShoulder;
            KeyRS.Text = _config.KeybindRightShoulder;
            KeyLT.Text = _config.KeybindLeftTrigger;
            KeyRT.Text = _config.KeybindRightTrigger;
            KeyLUp.Text = _config.KeybindLStickUp;
            KeyLDown.Text = _config.KeybindLStickDown;
            KeyLLeft.Text = _config.KeybindLStickLeft;
            KeyLRight.Text = _config.KeybindLStickRight;
            KeyLPress.Text = _config.KeybindLStickPress;
            KeyRUp.Text = _config.KeybindRStickUp;
            KeyRDown.Text = _config.KeybindRStickDown;
            KeyRLeft.Text = _config.KeybindRStickLeft;
            KeyRRight.Text = _config.KeybindRStickRight;
            KeyRPress.Text = _config.KeybindRStickPress;
            KeyDUp.Text = _config.KeybindDpadUp;
            KeyDDown.Text = _config.KeybindDpadDown;
            KeyDLeft.Text = _config.KeybindDpadLeft;
            KeyDRight.Text = _config.KeybindDpadRight;
        }

        private void SaveKeybindsToConfig()
        {
            _config.KeybindA = KeyA.Text;
            _config.KeybindB = KeyB.Text;
            _config.KeybindX = KeyX.Text;
            _config.KeybindY = KeyY.Text;
            _config.KeybindBack = KeyBack.Text;
            _config.KeybindStart = KeyStart.Text;
            _config.KeybindLeftShoulder = KeyLS.Text;
            _config.KeybindRightShoulder = KeyRS.Text;
            _config.KeybindLeftTrigger = KeyLT.Text;
            _config.KeybindRightTrigger = KeyRT.Text;
            _config.KeybindLStickUp = KeyLUp.Text;
            _config.KeybindLStickDown = KeyLDown.Text;
            _config.KeybindLStickLeft = KeyLLeft.Text;
            _config.KeybindLStickRight = KeyLRight.Text;
            _config.KeybindLStickPress = KeyLPress.Text;
            _config.KeybindRStickUp = KeyRUp.Text;
            _config.KeybindRStickDown = KeyRDown.Text;
            _config.KeybindRStickLeft = KeyRLeft.Text;
            _config.KeybindRStickRight = KeyRRight.Text;
            _config.KeybindRStickPress = KeyRPress.Text;
            _config.KeybindDpadUp = KeyDUp.Text;
            _config.KeybindDpadDown = KeyDDown.Text;
            _config.KeybindDpadLeft = KeyDLeft.Text;
            _config.KeybindDpadRight = KeyDRight.Text;
        }

        private void KeyField_PreviewKeyDown(object sender, System.Windows.Input.KeyEventArgs e)
        {
            e.Handled = true;
            var box = sender as System.Windows.Controls.TextBox;
            if (box == null) return;

            var key = e.Key == System.Windows.Input.Key.System ? e.SystemKey : e.Key;
            string keyName = KeybindNames.FromWpfKey(key);
            if (keyName == null) return;

            if (keyName == "Shift" || keyName == "Ctrl" || keyName == "Alt")
            {
                box.Text = keyName;
                return;
            }

            box.Text = KeybindNames.ModifierPrefix() + keyName;
        }

        private void KeyField_PreviewMouseDown(object sender, System.Windows.Input.MouseButtonEventArgs e)
        {
            var box = e.OriginalSource as System.Windows.Controls.TextBox;
            if (box == null) return;
            if (e.ChangedButton == System.Windows.Input.MouseButton.Left && !box.IsFocused)
                return;

            string name = KeybindNames.FromWpfMouseButton(e.ChangedButton);
            if (name == null) return;

            box.Text = KeybindNames.ModifierPrefix() + name;
            e.Handled = true;
        }

        private void ResetKeybinds_Click(object sender, RoutedEventArgs e)
        {
            _config.ResetKeybinds();
            PopulateKeybinds();
            MessageBox.Show("Default bindings restored.", "Reset",
                MessageBoxButton.OK, MessageBoxImage.Information);
        }

        private void SaveKeybinds_Click(object sender, RoutedEventArgs e)
        {
            SaveKeybindsToConfig();
            SaveConfig();
            MessageBox.Show("Keyboard bindings saved. They will be applied when you click PLAY.", "Saved",
                MessageBoxButton.OK, MessageBoxImage.Information);
        }

        private void RefreshPlayStatus()
        {
            string exePath = PathHelper.GetGameExecutablePath(_installDir);
            string gameData = _config.GameDataRoot ?? PathHelper.GetGameDataPath(_installDir);
            if (File.Exists(exePath) && File.Exists(Path.Combine(gameData, "default.xex")))
            {
                PlayStatusText.Text = Lt(LauncherLocalizer.PlayStatusReady);
                string release = IsoChecker.InstalledRelease(gameData);
                if (release != null)
                    PlayStatusText.Text = PlayStatusText.Text.TrimEnd('.') + ": " + release;
                StatusGameDot.Fill = (Brush)FindResource("SuccessBrush");
            }
            else
            {
                PlayStatusText.Text = Lt(LauncherLocalizer.PlayStatusMissing);
                StatusGameDot.Fill = (Brush)FindResource("DangerBrush");
            }
        }

        // Logs kept from earlier sessions; the session being started adds one.
        private const int KeptLogFiles = 9;

        private void ClearOldLogs()
        {
            try
            {
                string logsDir = PathHelper.GetLogsPath(_installDir);
                if (!Directory.Exists(logsDir))
                    return;

                var old = Directory.GetFiles(logsDir, "*.log", SearchOption.AllDirectories)
                    .OrderByDescending(f => File.GetLastWriteTimeUtc(f))
                    .Skip(KeptLogFiles);
                foreach (var file in old)
                {
                    try { File.Delete(file); } catch { }
                }
            }
            catch { }
        }

        // False (with a message) when the typed resolution is not WIDTHxHEIGHT.
        private bool SaveSettingsToConfig()
        {
            if (!DisplayOptions.TryParseResolution(ResolutionCombo.Text, out int width, out int height))
            {
                MessageBox.Show(string.Format(CultureInfo.InvariantCulture,
                        "'{0}' is not a resolution. Use WIDTHxHEIGHT, for example 2560x1440 ({1} to {2} pixels per side).",
                        ResolutionCombo.Text, DisplayOptions.MinResolution, DisplayOptions.MaxResolution),
                    "Resolution", MessageBoxButton.OK, MessageBoxImage.Warning);
                return false;
            }
            _config.RenderResolution = DisplayOptions.FormatResolution(width, height);
            ResolutionCombo.Text = _config.RenderResolution;

            _config.Aspect = AspectCombo.SelectedValue as string;
            if (FrameRateCombo.SelectedValue is int frameRate)
                _config.FrameRate = frameRate;

            _config.ShowFpsOverlay = FpsOverlayCheck.IsChecked ?? false;
            _config.Smaa = SmaaCheck.IsChecked ?? true;
            if (ShadowCombo.SelectedValue is int shadowScale)
                _config.ShadowScale = shadowScale;

            int anisoIdx = AnisoCombo.SelectedIndex;
            _config.AnisotropicOverride = anisoIdx <= 0 ? -1 : AnisoLevels[anisoIdx];

            _config.Fullscreen = FullscreenCheck.IsChecked ?? true;

            _config.InputBackend = (ControllerFixCheck.IsChecked ?? false) ? "sdl" : "xinput";

            _config.GlyphFamily = GlyphFamilyCombo.SelectedValue as string ?? "auto";

            _config.DetailedLogging = LoggingEnabledCheck.IsChecked ?? false;

            if (LanguageCombo.SelectedValue is uint langId)
                _config.UserLanguage = langId;

            _config["dlc_source_path"] = PathHelper.GetDlcPath(_installDir);
            return ApplyPortableMode();
        }

        // portable.txt on or off, from the checkbox. Turning it on copies the
        // saves from Documents when the game folder has none yet.
        private bool ApplyPortableMode()
        {
            bool want = PortableCheck.IsChecked ?? false;
            string marker = PathHelper.GetPortableMarkerPath(_installDir);
            if (want == File.Exists(marker))
                return true;
            string portableData = PathHelper.GetPortableDataPath(_installDir);
            try
            {
                if (want)
                {
                    string documents = PathHelper.GetDocumentsDataPath();
                    if (Directory.Exists(documents) && !Directory.Exists(portableData))
                        CopyDirectory(documents, portableData);
                    File.WriteAllText(marker,
                        "Portable mode: saves and settings are kept in the userdata folder next to this file.\r\n" +
                        "Delete this file (or untick Portable mode in the launcher) to use Documents again.\r\n");
                    MessageBox.Show(Lf(LauncherLocalizer.PortableEnabled, portableData), "Dante's Inferno",
                        MessageBoxButton.OK, MessageBoxImage.Information);
                }
                else
                {
                    File.Delete(marker);
                    MessageBox.Show(Lf(LauncherLocalizer.PortableDisabled, portableData), "Dante's Inferno",
                        MessageBoxButton.OK, MessageBoxImage.Information);
                }
                return true;
            }
            catch (Exception ex)
            {
                MessageBox.Show(Lf(LauncherLocalizer.PortableFailed, ex.Message), "Dante's Inferno",
                    MessageBoxButton.OK, MessageBoxImage.Error);
                PortableCheck.IsChecked = File.Exists(marker);
                return false;
            }
        }

        private static void CopyDirectory(string source, string destination)
        {
            Directory.CreateDirectory(destination);
            foreach (string file in Directory.GetFiles(source))
                File.Copy(file, Path.Combine(destination, Path.GetFileName(file)), true);
            foreach (string dir in Directory.GetDirectories(source))
                CopyDirectory(dir, Path.Combine(destination, Path.GetFileName(dir)));
        }

        // The portable zip has no installer: game\ is filled from the user's ISO
        // with the bundled extract-xiso, like the installer does.
        private void OfferIsoImport(string gameData)
        {
            string tool = PathHelper.GetExtractXisoPath(_installDir);
            if (!File.Exists(tool))
            {
                MessageBox.Show("default.xex was not found in the game data folder:\n" + gameData +
                    "\n\nThe game files were not extracted from the ISO. Run the installer again and check extract-xiso.log in the install folder.",
                    "Missing Data", MessageBoxButton.OK, MessageBoxImage.Error);
                return;
            }
            if (MessageBox.Show(Lf(LauncherLocalizer.ImportIsoPrompt, gameData), Lt(LauncherLocalizer.ImportIsoTitle),
                    MessageBoxButton.YesNo, MessageBoxImage.Question) != MessageBoxResult.Yes)
                return;
            var dialog = new Microsoft.Win32.OpenFileDialog
            {
                Title = Lt(LauncherLocalizer.ImportIsoTitle),
                Filter = "Xbox 360 ISO (*.iso)|*.iso|All files (*.*)|*.*",
            };
            if (dialog.ShowDialog(this) != true)
                return;
            string iso = dialog.FileName;
            PlayButton.IsEnabled = false;
            PlayNoteText.Text = "Checking the ISO...";
            Task.Factory.StartNew(() => IsoChecker.Check(iso)).ContinueWith(task =>
            {
                IsoCheckResult check = task.Exception == null ? task.Result : null;
                if (check == null)
                {
                    check = new IsoCheckResult();
                    check.Report.Add("ISO: " + iso);
                    check.Problem = "The ISO could not be read: " + task.Exception.GetBaseException().Message;
                    check.Report.Add("Problem: " + check.Problem);
                }
                WriteIsoReport(check, null);
                if (!check.CanExtract)
                {
                    PlayButton.IsEnabled = true;
                    PlayNoteText.Text = "";
                    MessageBox.Show(check.Problem + "\n\nISO: " + iso, Lt(LauncherLocalizer.ImportIsoTitle),
                        MessageBoxButton.OK, MessageBoxImage.Error);
                    return;
                }
                if (!check.KnownRelease && MessageBox.Show(
                        "This ISO is Dante's Inferno, " + check.Release + ".\n\nThe port is built and tested with the Europe (En, De, It) " +
                        "release. Other releases should work with the bundled Title Update but are not tested.\n\nImport it?",
                        Lt(LauncherLocalizer.ImportIsoTitle), MessageBoxButton.YesNo, MessageBoxImage.Question) != MessageBoxResult.Yes)
                {
                    PlayButton.IsEnabled = true;
                    PlayNoteText.Text = "";
                    return;
                }
                ExtractIso(iso, gameData, check);
            }, TaskScheduler.FromCurrentSynchronizationContext());
        }

        private void WriteIsoReport(IsoCheckResult check, List<string> extractionProblems)
        {
            try
            {
                string logsDir = PathHelper.GetLogsPath(_installDir);
                Directory.CreateDirectory(logsDir);
                var lines = new List<string> { "ISO check " + DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss", CultureInfo.InvariantCulture) };
                lines.AddRange(check.Report);
                if (extractionProblems != null)
                {
                    lines.Add(extractionProblems.Count == 0
                        ? "Extracted files: all present with the right sizes"
                        : "Extracted files with problems:");
                    lines.AddRange(extractionProblems.Select(p => "  " + p));
                }
                File.WriteAllLines(Path.Combine(logsDir, "iso_check.log"), lines);
            }
            catch { }
        }

        private void ExtractIso(string iso, string gameData, IsoCheckResult check)
        {
            string tool = PathHelper.GetExtractXisoPath(_installDir);
            string logPath = Path.Combine(PathHelper.GetLogsPath(_installDir), "extract-xiso.log");
            PlayNoteText.Text = Lt(LauncherLocalizer.ImportIsoRunning);

            Task.Factory.StartNew(new Action(() =>
            {
                int exitCode = -1;
                var output = new StringBuilder();
                try
                {
                    Directory.CreateDirectory(gameData);
                    var psi = new ProcessStartInfo
                    {
                        FileName = tool,
                        Arguments = "-x -d \"" + gameData + "\" -s \"" + iso + "\"",
                        WorkingDirectory = _installDir,
                        UseShellExecute = false,
                        CreateNoWindow = true,
                        RedirectStandardOutput = true,
                        RedirectStandardError = true,
                    };
                    using (var process = new Process { StartInfo = psi })
                    {
                        DataReceivedEventHandler append = (s, args) =>
                        {
                            if (args.Data != null)
                                lock (output) output.AppendLine(args.Data);
                        };
                        process.OutputDataReceived += append;
                        process.ErrorDataReceived += append;
                        process.Start();
                        process.BeginOutputReadLine();
                        process.BeginErrorReadLine();
                        process.WaitForExit();
                        exitCode = process.ExitCode;
                    }
                }
                catch (Exception ex)
                {
                    lock (output) output.AppendLine(ex.ToString());
                }
                try
                {
                    lock (output) File.WriteAllText(logPath, output.ToString());
                }
                catch { }
                var problems = IsoChecker.VerifyExtracted(check, gameData);
                check.Report.Add("extract-xiso exit code: " + exitCode);
                WriteIsoReport(check, problems);
                bool imported = problems.Count == 0;
                Dispatcher.Invoke(new Action(() =>
                {
                    if (imported)
                        EnsureTitleUpdate(gameData);
                    PlayButton.IsEnabled = true;
                    PlayNoteText.Text = imported ? Lt(LauncherLocalizer.ImportIsoDone) + " (" + check.Release + ")" : "";
                    if (!imported)
                    {
                        MessageBox.Show(Lf(LauncherLocalizer.ImportIsoFailed, exitCode) + "\n\n" +
                            string.Join("\n", problems.Take(8)) +
                            (problems.Count > 8 ? "\n..." : "") +
                            "\n\nCheck the free space on the drive and that no antivirus blocked the files. " +
                            "Details are in the logs folder (iso_check.log).",
                            Lt(LauncherLocalizer.ImportIsoTitle), MessageBoxButton.OK, MessageBoxImage.Error);
                    }
                    PopulateLanguageCombo();
                    RefreshPlayStatus();
                }));
            }));
        }

        // The recompiled code needs TU2; no other default.xexp matches it.
        private bool EnsureTitleUpdate(string gameData)
        {
            string target = PathHelper.GetTitleUpdatePath(gameData);
            string source = PathHelper.GetBundledTitleUpdatePath(_installDir);
            if (!File.Exists(source))
            {
                // Installs from before tu\ only have the copy in game\.
                string local = PathHelper.GetTitleUpdatePath(PathHelper.GetGameDataPath(_installDir));
                source = File.Exists(local) ? local : null;
            }
            try
            {
                if (source != null && !SameFile(source, target))
                    File.Copy(source, target, true);
            }
            catch (Exception ex)
            {
                MessageBox.Show("The Title Update (default.xexp) could not be copied to the game data folder:\n" +
                    gameData + "\n\n" + ex.Message, "Missing Title Update", MessageBoxButton.OK, MessageBoxImage.Error);
                return false;
            }
            RefreshDlcStatus();
            if (File.Exists(target))
                return true;
            MessageBox.Show("The Title Update (default.xexp) is missing from the game data folder:\n" + gameData +
                "\n\nThe game cannot start without it. Download the release again; it is in the tu folder.",
                "Missing Title Update", MessageBoxButton.OK, MessageBoxImage.Error);
            return false;
        }

        private static bool SameFile(string a, string b)
        {
            if (string.Equals(Path.GetFullPath(a), Path.GetFullPath(b), StringComparison.OrdinalIgnoreCase))
                return true;
            if (!File.Exists(b) || new FileInfo(a).Length != new FileInfo(b).Length)
                return false;
            using (var sha = System.Security.Cryptography.SHA1.Create())
            {
                byte[] ha, hb;
                using (var s = File.OpenRead(a)) ha = sha.ComputeHash(s);
                using (var s = File.OpenRead(b)) hb = sha.ComputeHash(s);
                return ha.SequenceEqual(hb);
            }
        }

        private void PlayButton_Click(object sender, RoutedEventArgs e)
        {
            if (_gameRunning)
            {
                MessageBox.Show("The game is already running.", "Already Running", MessageBoxButton.OK, MessageBoxImage.Information);
                return;
            }

            string exePath = PathHelper.GetGameExecutablePath(_installDir);
            if (!File.Exists(exePath))
            {
                MessageBox.Show("dantes_inferno.exe was not found in the install directory.", "Missing Game", MessageBoxButton.OK, MessageBoxImage.Error);
                return;
            }

            string gameData = _config.GameDataRoot ?? PathHelper.GetGameDataPath(_installDir);
            if (!File.Exists(Path.Combine(gameData, "default.xex")))
            {
                OfferIsoImport(gameData);
                return;
            }

            if (!EnsureTitleUpdate(gameData))
                return;

            if (!SaveSettingsToConfig())
                return;
            SaveKeybindsToConfig();
            SaveConfig();

            ClearOldLogs();

            string arguments = DisplayOptions.BuildLaunchArguments(_config, gameData, _displayAspect);

            PlayNoteText.Text = DisplayOptions.IsRendererFallback(_config)
                ? "Launching with the D3D12 fallback renderer (renderer_override)..."
                : string.Format(CultureInfo.InvariantCulture, "Launching at {0}, {1} FPS...",
                    _config.RenderResolution, DisplayOptions.FrameRateFor(_config, _displayAspect));
            PlayButton.IsEnabled = false;
            _gameRunning = true;

            Task.Factory.StartNew(new Action(() =>
            {
                int exitCode = -1;
                bool started = false;
                var errors = new List<string>();
                var attempts = new[]
                {
                    new ProcessStartInfo { FileName = exePath, Arguments = arguments, WorkingDirectory = _installDir, UseShellExecute = false },
                    new ProcessStartInfo { FileName = exePath, Arguments = arguments, WorkingDirectory = _installDir, UseShellExecute = true },
                    new ProcessStartInfo { FileName = Path.GetFileName(exePath), Arguments = arguments, WorkingDirectory = _installDir, UseShellExecute = true },
                };
                foreach (var psi in attempts)
                {
                    try
                    {
                        using (var process = Process.Start(psi))
                        {
                            if (process == null)
                            {
                                errors.Add("no process returned");
                                continue;
                            }
                            started = true;
                            process.WaitForExit();
                            exitCode = process.ExitCode;
                        }
                        break;
                    }
                    catch (Exception ex)
                    {
                        errors.Add(ex.Message);
                    }
                }

                Dispatcher.Invoke(new Action(() =>
                {
                    _gameRunning = false;
                    PlayButton.IsEnabled = true;

                    if (_autoPlay && started && exitCode == 0)
                    {
                        Close();
                        return;
                    }
                    if (_autoPlay)
                        ShowLauncher();
                    if (!started)
                        ShowLaunchFailed(exePath, arguments, errors);
                    else if (exitCode != 0)
                        ShowCrashDialog(exitCode);
                }));
            }));
        }

        private void ShowLauncher()
        {
            ShowInTaskbar = true;
            WindowState = WindowState.Normal;
            Activate();
        }

        private void ShowLaunchFailed(string exePath, string arguments, List<string> errors)
        {
            string message = "The launcher could not start the game:\n" + string.Join("\n", errors.Distinct()) +
                "\n\nThis happens on Linux / Steam Deck (Proton), which is not officially supported yet. " +
                "You can start dantes_inferno.exe directly with the launcher's settings instead: add it to Steam " +
                "and paste the launch options into its properties.\n\nCopy the launch options to the clipboard?";
            if (MessageBox.Show(message, "Launch Error", MessageBoxButton.YesNo, MessageBoxImage.Error) != MessageBoxResult.Yes)
                return;
            try
            {
                Clipboard.SetText(arguments);
                MessageBox.Show("Launch options copied. Game executable:\n" + exePath, "Launch Error",
                    MessageBoxButton.OK, MessageBoxImage.Information);
            }
            catch (Exception ex)
            {
                MessageBox.Show("Copying failed: " + ex.Message + "\n\nLaunch options:\n" + arguments, "Launch Error",
                    MessageBoxButton.OK, MessageBoxImage.Warning);
            }
        }

        private void ShowCrashDialog(int exitCode)
        {
            string logsDir = PathHelper.GetLogsPath(_installDir);
            string latestLog = null;
            try
            {
                if (Directory.Exists(logsDir))
                    latestLog = Directory.GetFiles(logsDir, "*.log", SearchOption.AllDirectories)
                        .OrderByDescending(File.GetLastWriteTime)
                        .FirstOrDefault();
            }
            catch { }

            string message = "Dante's Inferno has crashed (exit code: " + exitCode + ").\n\n";
            if (latestLog != null)
                message += "Log file: " + latestLog + "\n\n";
            else
                message += "No log file was found in " + logsDir + "\n\n";
            message += "Please report this on GitHub and attach the log file.\n";
            message += IssuesUrl + "\n\n";
            message += "Click OK to open the log folder and the GitHub issues page.";

            if (MessageBox.Show(message, "Game Crashed", MessageBoxButton.OKCancel, MessageBoxImage.Error) == MessageBoxResult.OK)
                OpenProblemReport();
        }

        private void ReportProblem_Click(object sender, RoutedEventArgs e)
        {
            OpenProblemReport();
        }

        private void ExportSaves_Click(object sender, RoutedEventArgs e)
        {
            var roots = new List<KeyValuePair<string, string>>
            {
                new KeyValuePair<string, string>("Portable", PathHelper.GetPortableDataPath(_installDir)),
                new KeyValuePair<string, string>("Documents", PathHelper.GetDocumentsDataPath()),
                new KeyValuePair<string, string>("LocalAppData", Path.Combine(
                    Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "dantes_inferno")),
            };
            var profiles = new List<KeyValuePair<string, string>>();
            foreach (var root in roots)
            {
                if (!Directory.Exists(root.Value))
                    continue;
                foreach (string dir in Directory.GetDirectories(root.Value))
                {
                    string name = Path.GetFileName(dir);
                    if (name.Length == 16 && name != "0000000000000000" &&
                        name.All(c => Uri.IsHexDigit(c)))
                        profiles.Add(new KeyValuePair<string, string>(root.Key, dir));
                }
            }
            if (profiles.Count == 0)
            {
                MessageBox.Show("No save files were found.", "Export Saves", MessageBoxButton.OK, MessageBoxImage.Information);
                return;
            }

            var dialog = new Microsoft.Win32.SaveFileDialog
            {
                Title = "Export Saves",
                FileName = "dantes_inferno_saves_" + DateTime.Now.ToString("yyyyMMdd_HHmm", CultureInfo.InvariantCulture) + ".zip",
                Filter = "Zip archive (*.zip)|*.zip",
                InitialDirectory = Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory),
            };
            if (dialog.ShowDialog(this) != true)
                return;
            string zip = dialog.FileName;

            string staging = Path.Combine(Path.GetTempPath(), "dantes_inferno_saves_" + Guid.NewGuid().ToString("N"));
            try
            {
                foreach (var profile in profiles)
                    CopyDirectory(profile.Value, Path.Combine(staging, profile.Key, Path.GetFileName(profile.Value)));
                if (File.Exists(zip))
                    File.Delete(zip);
                var psi = new ProcessStartInfo
                {
                    FileName = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "tar.exe"),
                    Arguments = "-a -c -f \"" + zip + "\" " + string.Join(" ",
                        profiles.Select(p => p.Key).Distinct().Select(k => "\"" + k + "\"")),
                    WorkingDirectory = staging,
                    UseShellExecute = false,
                    CreateNoWindow = true,
                };
                using (var process = Process.Start(psi))
                {
                    process.WaitForExit();
                    if (process.ExitCode != 0 || !File.Exists(zip))
                        throw new IOException("tar exited with code " + process.ExitCode);
                }
                OpenInExplorer("/select,\"" + zip + "\"");
            }
            catch (Exception ex)
            {
                MessageBox.Show("The saves could not be exported:\n" + ex.Message, "Export Saves",
                    MessageBoxButton.OK, MessageBoxImage.Error);
            }
            finally
            {
                try
                {
                    if (Directory.Exists(staging))
                        Directory.Delete(staging, true);
                }
                catch { }
            }
        }

        private void OpenProblemReport()
        {
            string logsDir = PathHelper.GetLogsPath(_installDir);
            string zip = Path.Combine(logsDir, "report.zip");
            bool zipped = false;
            try
            {
                var logs = Directory.Exists(logsDir)
                    ? Directory.GetFiles(logsDir, "*.log")
                        .Where(f => Path.GetFileName(f).StartsWith("dantes_inferno", StringComparison.OrdinalIgnoreCase))
                        .OrderByDescending(f => File.GetLastWriteTime(f))
                        .Take(3)
                        .Concat(new[] { "iso_check.log", "extract-xiso.log" }
                            .Select(n => Path.Combine(logsDir, n))
                            .Where(File.Exists))
                        .Select(Path.GetFileName)
                        .ToList()
                    : new List<string>();
                if (logs.Count > 0)
                {
                    if (File.Exists(zip))
                        File.Delete(zip);
                    var psi = new ProcessStartInfo
                    {
                        FileName = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "tar.exe"),
                        Arguments = "-a -c -f report.zip " + string.Join(" ", logs.Select(l => "\"" + l + "\"")),
                        WorkingDirectory = logsDir,
                        UseShellExecute = false,
                        CreateNoWindow = true,
                    };
                    using (var process = Process.Start(psi))
                    {
                        process.WaitForExit(10000);
                        zipped = process.HasExited && process.ExitCode == 0 && File.Exists(zip);
                    }
                }
            }
            catch { }

            try
            {
                if (zipped)
                    OpenInExplorer("/select,\"" + zip + "\"");
                else if (Directory.Exists(logsDir))
                    OpenInExplorer("\"" + logsDir + "\"");
                else
                    OpenInExplorer("\"" + _installDir + "\"");
            }
            catch { }

            try
            {
                OpenUrl(IssuesUrl + "/new");
            }
            catch { }

            MessageBox.Show(zipped
                    ? "Your latest logs are in report.zip (selected in the folder that just opened).\n\nDrag it into the GitHub issue and describe what happened."
                    : "Attach the newest .log file from the folder that just opened to the GitHub issue and describe what happened.",
                "Report a Problem", MessageBoxButton.OK, MessageBoxImage.Information);
        }

        private void ExitButton_Click(object sender, RoutedEventArgs e)
        {
            Close();
        }

        private void MinimizeButton_Click(object sender, RoutedEventArgs e)
        {
            WindowState = WindowState.Minimized;
        }

        private void TitleBar_MouseLeftButtonDown(object sender, System.Windows.Input.MouseButtonEventArgs e)
        {
            if (e.ButtonState == System.Windows.Input.MouseButtonState.Pressed)
                DragMove();
        }

        private void Nav_Checked(object sender, RoutedEventArgs e)
        {
            if (MainTabControl == null)
                return;
            if (!int.TryParse((sender as FrameworkElement)?.Tag as string, out int index))
                return;
            if (MainTabControl.SelectedItem == TabSettings || MainTabControl.SelectedItem == TabControls)
                AutoSave();
            MainTabControl.SelectedIndex = index;
        }

        private void ChangeSettings_Click(object sender, RoutedEventArgs e)
        {
            NavSettings.IsChecked = true;
        }

        private void SaveConfig()
        {
            string gameData = _config.GameDataRoot ?? PathHelper.GetGameDataPath(_installDir);
            _config.ApplyGameSettings(DisplayOptions.BuildGameSettings(_config, gameData, _displayAspect));
            _config.Save();
        }

        private bool AutoSave()
        {
            if (_config == null || !SaveSettingsToConfig())
                return false;
            SaveKeybindsToConfig();
            SaveConfig();
            RefreshSettingsSummary();
            return true;
        }

        private void RefreshSettingsSummary()
        {
            var parts = new List<string>();
            if (DisplayOptions.IsRendererFallback(_config))
                parts.Add("D3D12 fallback");
            else
                parts.Add(DisplayOptions.TryParseResolution(_config.RenderResolution, out int width, out int height)
                    ? DisplayOptions.FormatResolution(width, height)
                    : DisplayOptions.NativeResolution(_displayAspect));
            parts.Add(DisplayOptions.FrameRateFor(_config, _displayAspect) + " FPS");
            if (_config.Smaa)
                parts.Add("SMAA");
            if (!_config.Fullscreen)
                parts.Add("Windowed");
            SettingsSummaryText.Text = string.Join("  \u00B7  ", parts);
        }

        private void InstallDlc_Click(object sender, RoutedEventArgs e)
        {
            var dialog = new Microsoft.Win32.OpenFileDialog
            {
                Title = Lt(LauncherLocalizer.InstallDlc),
                Multiselect = true,
                Filter = "DLC packages and archives|*.*",
            };
            if (dialog.ShowDialog(this) == true)
                InstallDlc(dialog.FileNames);
        }

        private void DlcTab_DragOver(object sender, DragEventArgs e)
        {
            e.Effects = e.Data.GetDataPresent(DataFormats.FileDrop) ? DragDropEffects.Copy : DragDropEffects.None;
            e.Handled = true;
        }

        private void DlcTab_Drop(object sender, DragEventArgs e)
        {
            var paths = e.Data.GetData(DataFormats.FileDrop) as string[];
            if (paths != null && paths.Length > 0)
                InstallDlc(paths);
        }

        private void InstallDlc(string[] paths)
        {
            if (_gameRunning)
            {
                MessageBox.Show("Close the game before installing DLC.", "Install DLC", MessageBoxButton.OK, MessageBoxImage.Information);
                return;
            }
            string dlcRoot = PathHelper.GetDlcPath(_installDir);
            InstallDlcButton.IsEnabled = false;
            PlayButton.IsEnabled = false;
            DlcStatusText.Text = "Checking DLC packages...";
            Task.Factory.StartNew(() => DlcInstaller.Install(paths, dlcRoot)).ContinueWith(task =>
            {
                InstallDlcButton.IsEnabled = true;
                PlayButton.IsEnabled = !_gameRunning;
                RefreshDlcStatus();
                if (task.Exception != null)
                {
                    MessageBox.Show("Installing DLC failed:\n" + task.Exception.GetBaseException().Message, "Install DLC",
                        MessageBoxButton.OK, MessageBoxImage.Error);
                    return;
                }
                ShowDlcInstallResult(task.Result);
            }, TaskScheduler.FromCurrentSynchronizationContext());
        }

        private void ShowDlcInstallResult(DlcInstallResult result)
        {
            var text = new StringBuilder();
            if (result.Installed.Count > 0)
                text.AppendLine("Installed:\n  " + string.Join("\n  ", result.Installed) + "\n");
            if (result.AlreadyInstalled.Count > 0)
                text.AppendLine("Already installed:\n  " + string.Join("\n  ", result.AlreadyInstalled) + "\n");
            if (result.Skipped.Count > 0)
                text.AppendLine("Skipped:\n  " + string.Join("\n  ", result.Skipped) + "\n");
            if (result.Errors.Count > 0)
                text.AppendLine("Problems:\n  " + string.Join("\n  ", result.Errors) + "\n");
            if (result.UnknownFiles > 0)
                text.AppendLine(result.UnknownFiles + " file(s) were not Xbox 360 packages and were ignored. " +
                    "DLC must be the original package files (names like 23471E7296C26144C5E0...), not folders extracted from them.\n");
            if (result.Installed.Count == 0 && result.AlreadyInstalled.Count == 0)
                text.AppendLine("No Dante's Inferno DLC was found in what you selected.");
            else if (result.Installed.Count > 0)
                text.AppendLine("The game installs new DLC the next time it starts.");
            if (result.Installed.Concat(result.AlreadyInstalled).Any(n => n.IndexOf("Lucia", StringComparison.OrdinalIgnoreCase) >= 0))
                text.AppendLine("\nTrials of St. Lucia installs, but its arena mode is not playable in the port yet.");
            MessageBox.Show(text.ToString().TrimEnd(), "Install DLC", MessageBoxButton.OK,
                result.Errors.Count > 0 ? MessageBoxImage.Warning : MessageBoxImage.Information);
        }

        private void OpenDlcFolder_Click(object sender, RoutedEventArgs e)
        {
            string dlcPath = PathHelper.GetDlcPath(_installDir);
            try
            {
                if (!Directory.Exists(dlcPath))
                    Directory.CreateDirectory(dlcPath);
                OpenInExplorer("\"" + dlcPath + "\"");
                RefreshDlcStatus();
            }
            catch (Exception ex)
            {
                MessageBox.Show("Could not open DLC folder:\n" + ex.Message, "Error", MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }

        private void OpenTuFolder_Click(object sender, RoutedEventArgs e)
        {
            string gameDir = _config.GameDataRoot ?? PathHelper.GetGameDataPath(_installDir);
            try
            {
                if (!Directory.Exists(gameDir))
                    Directory.CreateDirectory(gameDir);
                OpenInExplorer("\"" + gameDir + "\"");
                RefreshDlcStatus();
            }
            catch (Exception ex)
            {
                MessageBox.Show("Could not open game folder:\n" + ex.Message, "Error", MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }

        private void RefreshDlcStatus()
        {
            string dlcPath = PathHelper.GetDlcPath(_installDir);
            string tuPath = PathHelper.GetTitleUpdatePath(_config.GameDataRoot ?? PathHelper.GetGameDataPath(_installDir));

            var dlcNames = new List<string>();
            try
            {
                dlcNames = DlcInstaller.ListInstalled(dlcPath)
                    .Select(p => string.IsNullOrEmpty(p.DisplayName) ? Path.GetFileName(p.Path) : p.DisplayName)
                    .Distinct()
                    .OrderBy(n => n)
                    .ToList();
            }
            catch { }
            string dlcText = dlcNames.Count > 0
                ? Lf(LauncherLocalizer.DlcFolderFound, dlcNames.Count)
                : Lt(LauncherLocalizer.DlcFolderEmpty);
            bool tuReady = File.Exists(tuPath) || File.Exists(PathHelper.GetBundledTitleUpdatePath(_installDir));
            string tuText = Lt(tuReady ? LauncherLocalizer.TuInstalled : LauncherLocalizer.TuNotFound);

            DlcStatusText.Text = dlcText + string.Concat(dlcNames.Select(n => "\n  " + n)) + "\n" + tuText;
            StatusTuText.Text = tuText;
            StatusTuDot.Fill = (Brush)FindResource(tuReady ? "SuccessBrush" : "DangerBrush");
            StatusDlcText.Text = dlcText;
            StatusDlcDot.Fill = (Brush)FindResource(dlcNames.Count > 0 ? "SuccessBrush" : "TextMutedBrush");
        }

        private void ApplyRecommended_Click(object sender, RoutedEventArgs e)
        {
            _config.RenderResolution = DisplayOptions.NativeResolution(_displayAspect);
            _config.Aspect = DisplayOptions.AspectAuto;
            _config.FrameRate = Math.Min(DisplayOptions.DefaultFrameRate, DisplayOptions.MaxFrameRate(_displayAspect));
            _config.ShowFpsOverlay = false;
            _config.Smaa = true;
            _config.ShadowScale = DisplayOptions.DefaultShadowScale;
            _config.AnisotropicOverride = -1;
            _config.Fullscreen = true;
            _config.InputBackend = "sdl";
            PopulateControls();
            MessageBox.Show("Recommended settings applied.", "Recommended",
                MessageBoxButton.OK, MessageBoxImage.Information);
        }

        private void SaveSettings_Click(object sender, RoutedEventArgs e)
        {
            if (!SaveSettingsToConfig())
                return;
            SaveConfig();
            RefreshPlayStatus();
            MessageBox.Show("Settings saved. They will be applied when you click PLAY.", "Saved",
                MessageBoxButton.OK, MessageBoxImage.Information);
        }

        private ReleaseInfo _pendingUpdate;

        private static ReleaseAsset FindInstallerAsset(ReleaseInfo release)
        {
            return release.Assets.FirstOrDefault(
                a => a.Name.Equals("DantesInfernoInstaller.exe", StringComparison.OrdinalIgnoreCase));
        }

        private void CheckUpdates_Click(object sender, RoutedEventArgs e)
        {
            UpdateStatusText.Text = "Checking for updates...";
            DownloadUpdateButton.Visibility = Visibility.Collapsed;
            try
            {
                var release = GitHubUpdater.CheckLatest();
                var localVersion = GitHubUpdater.GetLocalVersion(_installDir);
                if (release.Version > localVersion)
                {
                    bool hasInstaller = FindInstallerAsset(release) != null;
                    _pendingUpdate = hasInstaller ? release : null;
                    DownloadUpdateButton.Visibility = hasInstaller ? Visibility.Visible : Visibility.Collapsed;
                    UpdateStatusText.Text =
                        $"Update available!\nVersion: {release.TagName}\nName: {release.Name}\n\n{release.Body}\n\n" +
                        (hasInstaller
                            ? "Click \"Download & Install\" to get the new installer."
                            : "No installer asset found in this release. Visit the releases page to download manually:\n" + release.HtmlUrl);
                }
                else
                {
                    _pendingUpdate = null;
                    UpdateStatusText.Text = $"You are up to date.\nLatest: {release.TagName}\nInstalled: {localVersion}";
                }
            }
            catch (Exception ex)
            {
                _pendingUpdate = null;
                UpdateStatusText.Text = "Failed to check for updates:\n" + ex.Message;
            }
        }

        private void DownloadUpdate_Click(object sender, RoutedEventArgs e)
        {
            if (_pendingUpdate == null)
            {
                MessageBox.Show("No update has been checked yet. Click \"Check for Updates\" first.",
                    "No Update", MessageBoxButton.OK, MessageBoxImage.Information);
                return;
            }

            // Unpacked from the portable zip (no uninstaller): the installer would
            // install a second copy, so the new zip is downloaded by hand.
            if (!File.Exists(Path.Combine(_installDir, "unins000.exe")) && !string.IsNullOrEmpty(_pendingUpdate.HtmlUrl))
            {
                MessageBox.Show($"A new version ({_pendingUpdate.TagName}) is available.\n\n" +
                    "This copy was unpacked from the portable zip: download the new portable zip from the release page " +
                    "and extract it over this folder (your saves and settings are kept).",
                    "Update Available", MessageBoxButton.OK, MessageBoxImage.Information);
                OpenUrl(_pendingUpdate.HtmlUrl);
                return;
            }

            var asset = FindInstallerAsset(_pendingUpdate);
            if (asset == null)
            {
                MessageBox.Show("The latest release has no installer asset to download.",
                    "No Installer", MessageBoxButton.OK, MessageBoxImage.Warning);
                return;
            }

            var confirm = MessageBox.Show(
                $"A new version ({_pendingUpdate.TagName}) is available.\n\n" +
                "The installer will be downloaded and launched. " +
                "The launcher will close so the installer can update your game files.\n\n" +
                "Continue?",
                "Download and Install Update",
                MessageBoxButton.YesNo, MessageBoxImage.Question);
            if (confirm != MessageBoxResult.Yes)
                return;

            DownloadUpdateButton.IsEnabled = false;
            CheckUpdatesButton.IsEnabled = false;
            DownloadProgress.Visibility = Visibility.Visible;
            DownloadProgress.Value = 0;
            UpdateStatusText.Text = "Downloading installer...";

            string destDir = Path.Combine(Path.GetTempPath(), "DantesInfernoUpdate");

            Task.Factory.StartNew(new Action(() =>
            {
                string downloadedPath = null;
                string errorMsg = null;
                try
                {
                    downloadedPath = GitHubUpdater.DownloadAsset(asset, destDir,
                        (received, total) =>
                        {
                            if (total > 0)
                            {
                                int pct = (int)(100L * received / total);
                                Dispatcher.Invoke(new Action(() =>
                                {
                                    DownloadProgress.Value = pct;
                                    UpdateStatusText.Text =
                                        $"Downloading... {pct}% ({received / 1024 / 1024} MB / {total / 1024 / 1024} MB)";
                                }));
                            }
                        });
                }
                catch (Exception ex)
                {
                    errorMsg = ex.Message;
                }

                Dispatcher.Invoke(new Action(() =>
                {
                    DownloadProgress.Value = 100;
                    DownloadUpdateButton.IsEnabled = true;
                    CheckUpdatesButton.IsEnabled = true;

                    if (errorMsg != null)
                    {
                        DownloadProgress.Visibility = Visibility.Collapsed;
                        UpdateStatusText.Text = "Download failed:\n" + errorMsg;
                        return;
                    }

                    UpdateStatusText.Text = "Download complete. Launching installer...";

                    var launch = MessageBox.Show(
                        "The installer has been downloaded. Click OK to launch it.\n" +
                        "The launcher will close and the installer will guide you through the update.",
                        "Launch Installer", MessageBoxButton.OKCancel, MessageBoxImage.Information);

                    if (launch != MessageBoxResult.OK)
                    {
                        DownloadProgress.Visibility = Visibility.Collapsed;
                        UpdateStatusText.Text = "Installer downloaded to:\n" + downloadedPath +
                            "\n\nYou can run it manually later.";
                        return;
                    }

                    try
                    {
                        OpenUrl(downloadedPath);
                        Application.Current.Shutdown();
                    }
                    catch (Exception ex)
                    {
                        DownloadProgress.Visibility = Visibility.Collapsed;
                        UpdateStatusText.Text = "Failed to launch installer:\n" + ex.Message +
                            "\n\nYou can run it manually from:\n" + downloadedPath;
                    }
                }));
            }));
        }

        private void SupportButton_Click(object sender, RoutedEventArgs e)
        {
            OpenUrl("https://ko-fi.com/zerkiller");
        }

        private static void OpenUrl(string url)
        {
            Process.Start(new ProcessStartInfo(url) { UseShellExecute = true });
        }

        private static void OpenInExplorer(string arguments)
        {
            Process.Start(new ProcessStartInfo("explorer.exe", arguments) { UseShellExecute = true });
        }
    }
}
