using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Media.Imaging;

namespace DantesInferno.Launcher
{
    public partial class MainWindow : Window
    {
        private GameConfig _config;
        private string _installDir;
        private bool _gameRunning;
        private DisplayAspect _displayAspect;
        private List<DisplayModeOption> _resolutionOptions;
        private string _launcherLanguage = LauncherLocalizer.DefaultLanguage;
        private bool _populatingLauncherLanguage;

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
            CheckForUpdatesOnStartup();
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

                var asset = release.Assets.FirstOrDefault(
                    a => a.Name.Equals("DantesInfernoInstaller.exe",
                        StringComparison.OrdinalIgnoreCase));
                if (asset == null)
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

            if (string.IsNullOrWhiteSpace(_config.GameDataRoot))
                _config.GameDataRoot = PathHelper.GetGameDataPath(_installDir);

            _config.MigrateLegacySettings();
            _displayAspect = DisplayOptions.DetectPrimaryAspect();
        }

        private void PopulateControls()
        {
            var version = GitHubUpdater.GetLocalVersion(_installDir);
            VersionText.Text = "Version: " + version.ToString();
            UpdateVersionText.Text = "Installed version: " + version.ToString();

            if (_displayAspect == null)
                _displayAspect = DisplayOptions.DetectPrimaryAspect();

            DetectedDisplayText.Text = string.Format(CultureInfo.InvariantCulture,
                "{0}x{1} detected ({2})",
                _displayAspect.DetectedWidth, _displayAspect.DetectedHeight, _displayAspect.Name);

            _resolutionOptions = DisplayOptions.BuildResolutionOptions(_displayAspect);
            ResolutionCombo.ItemsSource = _resolutionOptions;
            ResolutionCombo.DisplayMemberPath = "Label";
            int scale = DisplayOptions.ClampScale(_config.ResolutionScale);
            int scaleIndex = _resolutionOptions.FindIndex(o => o.Scale == scale);
            ResolutionCombo.SelectedIndex = scaleIndex < 0 ? 0 : scaleIndex;

            var rendererOptions = new List<KeyValuePair<string, string>>
            {
                new KeyValuePair<string, string>("ReXGlue (D3D12)", DisplayOptions.RendererReXGlue),
                new KeyValuePair<string, string>("Native (Vulkan)", DisplayOptions.RendererNative),
            };
            RendererCombo.ItemsSource = rendererOptions;
            RendererCombo.DisplayMemberPath = "Key";
            RendererCombo.SelectedValuePath = "Value";
            string renderer = DisplayOptions.NormalizeRenderer(_config.Renderer);
            RendererCombo.SelectedIndex = renderer == DisplayOptions.RendererNative ? 1 : 0;

            var accuracyOptions = new List<KeyValuePair<string, string>>
            {
                new KeyValuePair<string, string>("Automatic (host render targets)", ""),
                new KeyValuePair<string, string>("EDRAM (FSI)", "fsi"),
            };
            RenderAccuracyCombo.ItemsSource = accuracyOptions;
            RenderAccuracyCombo.DisplayMemberPath = "Key";
            RenderAccuracyCombo.SelectedValuePath = "Value";
            string accuracy = _config.VulkanRenderPath ?? "";
            int accuracyIndex = accuracyOptions.FindIndex(o => o.Value == accuracy);
            RenderAccuracyCombo.SelectedIndex = accuracyIndex < 0 ? 0 : accuracyIndex;

            AAModeCombo.ItemsSource = new List<string> { "Off", "FXAA", "FXAA Extreme" };
            switch (_config.SwapPostEffect)
            {
                case "none": AAModeCombo.SelectedIndex = 0; break;
                case "fxaa": AAModeCombo.SelectedIndex = 1; break;
                case "fxaa_extreme": AAModeCombo.SelectedIndex = 2; break;
                default: AAModeCombo.SelectedIndex = 0; break;
            }

            var postProcessingOptions = new List<KeyValuePair<string, string>>
            {
                new KeyValuePair<string, string>("Bilinear (default)", "bilinear"),
                new KeyValuePair<string, string>("CAS (sharpen)", "cas_sharpen"),
                new KeyValuePair<string, string>("FSR EASU (upscale)", "fsr_easu"),
                new KeyValuePair<string, string>("FSR RCAS (sharpen)", "fsr_rcas"),
            };
            PostProcessingCombo.ItemsSource = postProcessingOptions;
            PostProcessingCombo.DisplayMemberPath = "Key";
            PostProcessingCombo.SelectedValuePath = "Value";
            string presentEffect = DisplayOptions.NormalizePresentEffect(_config.PresentEffect);
            int ppIndex = postProcessingOptions.FindIndex(o => o.Value == presentEffect);
            PostProcessingCombo.SelectedIndex = ppIndex < 0 ? 0 : ppIndex;

            AnisoCombo.ItemsSource = new List<string> { "Default", "1x", "2x", "4x", "8x", "16x" };
            int aniso = _config.AnisotropicOverride;
            if (aniso < 0) aniso = 0;
            else aniso = Array.IndexOf(new[] { 0, 1, 2, 4, 8, 16 }, aniso);
            if (aniso < 0) aniso = 0;
            AnisoCombo.SelectedIndex = aniso;

            FullscreenCheck.IsChecked = _config.Fullscreen;
            VSyncCheck.IsChecked = _config.VSync;
            FpsOverlayCheck.IsChecked = _config.ShowFpsOverlay;
            DitherCheck.IsChecked = _config.PresentDither;

            ControllerFixCheck.IsChecked = _config.InputBackend.Equals("sdl", StringComparison.OrdinalIgnoreCase);

            GlyphFamilyCombo.SelectedIndex = _config.GlyphFamily.Equals("playstation", StringComparison.OrdinalIgnoreCase) ? 1 : 0;

            LoggingEnabledCheck.IsChecked = !_config.LogLevel.Equals("off", StringComparison.OrdinalIgnoreCase);

            PopulateLanguageCombo();
            PopulateLauncherLanguageCombo();
            PopulateKeybinds();
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
                _config.Save();
                ApplyLocalization();
            }
        }

        private void ApplyLocalization()
        {
            string Lt(string key) => LauncherLocalizer.Get(_launcherLanguage, key);
            string Lf(string key, params object[] args) => LauncherLocalizer.Get(_launcherLanguage, key, args);

            TabPlay.Header = Lt(LauncherLocalizer.TabPlay);
            TabDlc.Header = Lt(LauncherLocalizer.TabDlc);
            TabControls.Header = Lt(LauncherLocalizer.TabControls);
            TabSettings.Header = Lt(LauncherLocalizer.TabSettings);
            TabUpdates.Header = Lt(LauncherLocalizer.TabUpdates);

            PlaySubtitleText.Text = Lt(LauncherLocalizer.PlaySubtitle);
            PlayButton.Content = Lt(LauncherLocalizer.PlayButton);
            ExitButton.Content = Lt(LauncherLocalizer.ExitButton);
            SupportButton.Content = Lt(LauncherLocalizer.SupportDeveloper);
            var version = GitHubUpdater.GetLocalVersion(_installDir);
            VersionText.Text = Lf(LauncherLocalizer.VersionLabel, version.ToString());

            DlcTitleText.Text = Lt(LauncherLocalizer.DlcTitle);
            DlcDescriptionText.Text = Lt(LauncherLocalizer.DlcDescription);
            OpenDlcFolderButton.Content = Lt(LauncherLocalizer.OpenDlcFolder);
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
            LabelRenderer.Content = Lt(LauncherLocalizer.LabelRenderer);
            LabelAntiAliasing.Content = Lt(LauncherLocalizer.LabelAntiAliasing);
            LabelTextureFiltering.Content = Lt(LauncherLocalizer.LabelTextureFiltering);
            LabelGameLanguage.Content = Lt(LauncherLocalizer.LabelGameLanguage);
            LabelLauncherLanguage.Content = Lt(LauncherLocalizer.LabelLauncherLanguage);
            FullscreenCheck.Content = Lt(LauncherLocalizer.CheckFullscreen);
            GroupControlsSettings.Header = Lt(LauncherLocalizer.GroupControls);
            ControllerFixCheck.Content = Lt(LauncherLocalizer.CheckControllerBackend);
            ControllerFixCheck.ToolTip = Lt(LauncherLocalizer.ControllerTooltip);
            LabelButtonGlyphs.Text = Lt(LauncherLocalizer.LabelButtonGlyphs);
            ComingSoonText.Text = Lt(LauncherLocalizer.ComingSoon);
            LoggingEnabledCheck.Content = Lt(LauncherLocalizer.CheckLogging);
            SettingsAppliedNoteText.Text = Lt(LauncherLocalizer.SettingsAppliedNote);
            ApplyRecommendedButton.Content = Lt(LauncherLocalizer.ResetToRecommended);
            SaveSettingsButton.Content = Lt(LauncherLocalizer.SaveSettings);
            ResolutionCombo.ToolTip = Lt(LauncherLocalizer.ResolutionTooltip);
            RendererCombo.ToolTip = Lt(LauncherLocalizer.RendererTooltip);
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
            _config.KeybindA = "Space";
            _config.KeybindB = "F";
            _config.KeybindX = "LMB";
            _config.KeybindY = "E";
            _config.KeybindLeftShoulder = "Q";
            _config.KeybindRightShoulder = "RMB";
            _config.KeybindLeftTrigger = "Shift";
            _config.KeybindRightTrigger = "Ctrl";
            _config.KeybindLStickUp = "W";
            _config.KeybindLStickDown = "S";
            _config.KeybindLStickLeft = "A";
            _config.KeybindLStickRight = "D";
            _config.KeybindLStickPress = "X";
            _config.KeybindRStickUp = "Up";
            _config.KeybindRStickDown = "Down";
            _config.KeybindRStickLeft = "Left";
            _config.KeybindRStickRight = "Right";
            _config.KeybindRStickPress = "R";
            _config.KeybindDpadUp = "Shift+Up";
            _config.KeybindDpadDown = "Shift+Down";
            _config.KeybindDpadLeft = "Shift+Left";
            _config.KeybindDpadRight = "Shift+Right";
            _config.KeybindBack = "Tab";
            _config.KeybindStart = "Escape";
            PopulateKeybinds();
            MessageBox.Show("Default bindings restored. Click Save Bindings to keep them.", "Reset",
                MessageBoxButton.OK, MessageBoxImage.Information);
        }

        private void SaveKeybinds_Click(object sender, RoutedEventArgs e)
        {
            SaveKeybindsToConfig();
            _config.Save();
            MessageBox.Show("Keyboard bindings saved. They will be applied when you click PLAY.", "Saved",
                MessageBoxButton.OK, MessageBoxImage.Information);
        }

        private void RefreshPlayStatus()
        {
            string exePath = PathHelper.GetGameExecutablePath(_installDir);
            string gameData = _config.GameDataRoot ?? PathHelper.GetGameDataPath(_installDir);
            if (File.Exists(exePath) && Directory.Exists(gameData))
                PlayStatusText.Text = LauncherLocalizer.Get(_launcherLanguage, LauncherLocalizer.PlayStatusReady);
            else
                PlayStatusText.Text = LauncherLocalizer.Get(_launcherLanguage, "play_status_missing");
        }

        private void ClearOldLogs()
        {
            try
            {
                string logsDir = PathHelper.GetLogsPath(_installDir);
                if (!Directory.Exists(logsDir))
                    return;

                foreach (var file in Directory.GetFiles(logsDir, "*.log", SearchOption.AllDirectories))
                {
                    try { File.Delete(file); } catch { }
                }
            }
            catch { }
        }

        private void SaveSettingsToConfig()
        {
            var selectedMode = ResolutionCombo.SelectedItem as DisplayModeOption;
            _config.ResolutionScale = selectedMode != null
                ? DisplayOptions.ClampScale(selectedMode.Scale)
                : DisplayOptions.MinScale;

            var rendererPair = RendererCombo.SelectedItem as KeyValuePair<string, string>?;
            _config.Renderer = DisplayOptions.NormalizeRenderer(
                rendererPair.HasValue ? rendererPair.Value.Value : null);

            var accuracyPair = RenderAccuracyCombo.SelectedItem as KeyValuePair<string, string>?;
            _config.VulkanRenderPath = accuracyPair.HasValue ? accuracyPair.Value.Value : "";

            int aaIdx = AAModeCombo.SelectedIndex;
            switch (aaIdx)
            {
                case 1: _config.SwapPostEffect = "fxaa"; break;
                case 2: _config.SwapPostEffect = "fxaa_extreme"; break;
                default: _config.SwapPostEffect = "none"; break;
            }

            var postProcessingPair = PostProcessingCombo.SelectedItem as KeyValuePair<string, string>?;
            _config.PresentEffect = postProcessingPair.HasValue ? postProcessingPair.Value.Value : "bilinear";

            _config.PresentDither = DitherCheck.IsChecked ?? false;
            _config.ShowFpsOverlay = FpsOverlayCheck.IsChecked ?? false;

            int anisoIdx = AnisoCombo.SelectedIndex;
            if (anisoIdx <= 0)
                _config.AnisotropicOverride = -1;
            else
            {
                int[] anisoValues = { 0, 1, 2, 4, 8, 16 };
                _config.AnisotropicOverride = anisoValues[anisoIdx];
            }

            _config.Fullscreen = FullscreenCheck.IsChecked ?? true;
            _config.VSync = VSyncCheck.IsChecked ?? true;

            _config.InputBackend = (ControllerFixCheck.IsChecked ?? false) ? "sdl" : "xinput";

            _config.GlyphFamily = GlyphFamilyCombo.SelectedIndex == 1 ? "playstation" : "xbox";

            bool loggingEnabled = LoggingEnabledCheck.IsChecked ?? true;
            _config.LogLevel = loggingEnabled ? "info" : "off";

            if (LanguageCombo.SelectedValue is uint langId)
                _config.UserLanguage = langId;

            _config["dlc_source_path"] = PathHelper.GetDlcPath(_installDir);
        }

        private void PlayButton_Click(object sender, RoutedEventArgs e)
        {
            if (_gameRunning)
            {
                MessageBox.Show("The game is already running.", "Already Running", MessageBoxButton.OK, MessageBoxImage.Information);
                return;
            }

            string exePath = PathHelper.GetGameExecutablePath(_installDir);
            bool wantNative = DisplayOptions.NormalizeRenderer(_config.Renderer) == DisplayOptions.RendererNative;
            if (wantNative)
            {
                string nativeExe = PathHelper.GetNativeGameExecutablePath(_installDir);
                if (File.Exists(nativeExe))
                    exePath = nativeExe;
            }
            if (!File.Exists(exePath))
            {
                MessageBox.Show("dantes_inferno.exe was not found in the install directory.", "Missing Game", MessageBoxButton.OK, MessageBoxImage.Error);
                return;
            }

            string gameData = _config.GameDataRoot ?? PathHelper.GetGameDataPath(_installDir);
            if (!Directory.Exists(gameData))
            {
                MessageBox.Show("Game data folder was not found: " + gameData, "Missing Data", MessageBoxButton.OK, MessageBoxImage.Error);
                return;
            }

            SaveSettingsToConfig();
            SaveKeybindsToConfig();
            _config.Save();

            ClearOldLogs();

            string arguments = DisplayOptions.BuildLaunchArguments(_config, gameData, _displayAspect);

            var launchMode = ResolutionCombo.SelectedItem as DisplayModeOption;
            string rendererName = DisplayOptions.NormalizeRenderer(_config.Renderer) == DisplayOptions.RendererNative
                ? "Native (Vulkan)" : "ReXGlue (D3D12)";
            PlayNoteText.Text = "Launching at " +
                (launchMode != null ? launchMode.Width + "x" + launchMode.Height : "default resolution") +
                " using " + rendererName + "...";
            PlayButton.IsEnabled = false;
            _gameRunning = true;

            Task.Factory.StartNew(new Action(() =>
            {
                int exitCode = -1;
                try
                {
                    var psi = new ProcessStartInfo
                    {
                        FileName = exePath,
                        Arguments = arguments,
                        WorkingDirectory = _installDir,
                        UseShellExecute = false,
                    };

                    using (var process = Process.Start(psi))
                    {
                        process.WaitForExit();
                        exitCode = process.ExitCode;
                    }
                }
                catch (Exception ex)
                {
                    Dispatcher.Invoke(new Action(() =>
                    {
                        MessageBox.Show("Failed to start the game:\n" + ex.Message, "Launch Error", MessageBoxButton.OK, MessageBoxImage.Error);
                    }));
                }

                Dispatcher.Invoke(new Action(() =>
                {
                    _gameRunning = false;
                    PlayButton.IsEnabled = true;

                    if (exitCode != 0)
                        ShowCrashDialog(exitCode);
                }));
            }));
        }

        private void ShowCrashDialog(int exitCode)
        {
            string logsDir = PathHelper.GetLogsPath(_installDir);
            string latestLog = null;

            try
            {
                if (Directory.Exists(logsDir))
                {
                    var logFile = Directory.GetFiles(logsDir, "*.log", SearchOption.AllDirectories)
                        .OrderByDescending(f => File.GetLastWriteTime(f))
                        .FirstOrDefault();
                    if (logFile != null)
                        latestLog = logFile;
                }
            }
            catch { }

            string message = "Dante's Inferno has crashed (exit code: " + exitCode + ").\n\n";
            if (latestLog != null)
                message += "Log file: " + latestLog + "\n\n";
            else
                message += "No log file was found in " + logsDir + "\n\n";
            message += "Please report this on GitHub and attach the log file.\n";
            message += "https://github.com/florinp93/dantes-inferno/issues\n\n";
            message += "Click OK to open the log folder and the GitHub issues page.";

            var result = MessageBox.Show(message, "Game Crashed", MessageBoxButton.OKCancel, MessageBoxImage.Error);
            if (result == MessageBoxResult.OK)
            {
                try
                {
                    if (Directory.Exists(logsDir))
                        Process.Start(new ProcessStartInfo("explorer.exe", "\"" + logsDir + "\"") { UseShellExecute = true });
                    else
                        Process.Start(new ProcessStartInfo("explorer.exe", "\"" + _installDir + "\"") { UseShellExecute = true });
                }
                catch { }

                try
                {
                    Process.Start(new ProcessStartInfo("https://github.com/florinp93/dantes-inferno/issues") { UseShellExecute = true });
                }
                catch { }
            }
        }

        private void ExitButton_Click(object sender, RoutedEventArgs e)
        {
            Close();
        }

        private void OpenDlcFolder_Click(object sender, RoutedEventArgs e)
        {
            string dlcPath = PathHelper.GetDlcPath(_installDir);
            try
            {
                if (!Directory.Exists(dlcPath))
                    Directory.CreateDirectory(dlcPath);
                Process.Start(new ProcessStartInfo("explorer.exe", "\"" + dlcPath + "\"") { UseShellExecute = true });
                RefreshDlcStatus();
            }
            catch (Exception ex)
            {
                MessageBox.Show("Could not open DLC folder:\n" + ex.Message, "Error", MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }

        private void OpenTuFolder_Click(object sender, RoutedEventArgs e)
        {
            string gameDir = PathHelper.GetGameDataPath(_installDir);
            try
            {
                if (!Directory.Exists(gameDir))
                    Directory.CreateDirectory(gameDir);
                Process.Start(new ProcessStartInfo("explorer.exe", "\"" + gameDir + "\"") { UseShellExecute = true });
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
            string tuPath = PathHelper.GetTitleUpdatePath(_installDir);

            var parts = new List<string>();

            int dlcCount = 0;
            if (Directory.Exists(dlcPath))
            {
                try
                {
                    dlcCount = Directory.GetFiles(dlcPath, "*", SearchOption.AllDirectories)
                        .Count(f => !Path.GetFileName(f).Equals(".installed", StringComparison.OrdinalIgnoreCase));
                }
                catch { }
            }
            parts.Add(dlcCount > 0
                ? LauncherLocalizer.Get(_launcherLanguage, LauncherLocalizer.DlcFolderFound, dlcCount)
                : LauncherLocalizer.Get(_launcherLanguage, LauncherLocalizer.DlcFolderEmpty));

            parts.Add(File.Exists(tuPath)
                ? LauncherLocalizer.Get(_launcherLanguage, LauncherLocalizer.TuInstalled)
                : LauncherLocalizer.Get(_launcherLanguage, LauncherLocalizer.TuNotFound));

            DlcStatusText.Text = string.Join("\n", parts);
        }

        private void ApplyRecommended_Click(object sender, RoutedEventArgs e)
        {
            _config.ResolutionScale = 0;
            _config.Renderer = DisplayOptions.RendererNative;
            _config.VulkanRenderPath = "";
            _config.SwapPostEffect = "none";
            _config.PresentEffect = "cas_sharpen";
            _config.PresentDither = false;
            _config.ShowFpsOverlay = false;
            _config.AnisotropicOverride = -1;
            _config.VSync = true;
            _config.Fullscreen = true;
            _config.InputBackend = "sdl";
            PopulateControls();
            MessageBox.Show("Recommended settings applied. Click Save Settings to keep them.", "Recommended",
                MessageBoxButton.OK, MessageBoxImage.Information);
        }

        private void SaveSettings_Click(object sender, RoutedEventArgs e)
        {
            SaveSettingsToConfig();
            _config.Save();
            RefreshPlayStatus();
            MessageBox.Show("Settings saved. They will be applied when you click PLAY.", "Saved",
                MessageBoxButton.OK, MessageBoxImage.Information);
        }

        private ReleaseInfo _pendingUpdate;

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
                    var asset = release.Assets.FirstOrDefault(
                        a => a.Name.Equals("DantesInfernoInstaller.exe",
                            StringComparison.OrdinalIgnoreCase));

                    if (asset != null)
                    {
                        _pendingUpdate = release;
                        DownloadUpdateButton.Visibility = Visibility.Visible;
                        UpdateStatusText.Text =
                            $"Update available!\nVersion: {release.TagName}\nName: {release.Name}\n\n" +
                            $"{release.Body}\n\nClick \"Download & Install\" to get the new installer.";
                    }
                    else
                    {
                        UpdateStatusText.Text =
                            $"Update available!\nVersion: {release.TagName}\nName: {release.Name}\n\n" +
                            $"{release.Body}\n\nNo installer asset found in this release. " +
                            "Visit the releases page to download manually:\n" +
                            release.HtmlUrl;
                    }
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

            var asset = _pendingUpdate.Assets.FirstOrDefault(
                a => a.Name.Equals("DantesInfernoInstaller.exe",
                    StringComparison.OrdinalIgnoreCase));
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
                        Process.Start(new ProcessStartInfo
                        {
                            FileName = downloadedPath,
                            UseShellExecute = true,
                        });
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
            Process.Start(new ProcessStartInfo("https://ko-fi.com/zerkiller") { UseShellExecute = true });
        }

        private void TrialsServerButton_Click(object sender, RoutedEventArgs e)
        {
            Process.Start(new ProcessStartInfo("https://ko-fi.com/zerkiller") { UseShellExecute = true });
        }
    }
}
