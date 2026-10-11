# Dante's Inferno - Hell's Gate Launcher & Installer

C# WPF (.NET Framework 4.0) launcher plus an Inno Setup installer for the
ReXGlue PC port.

## Projects

- `DantesInferno.Shared` - settings (`dantes_inferno.toml`), the game's
  command line, disc language detection, GitHub release updater, UI text.
- `DantesInfernoLauncher` - the launcher: settings, controls, DLC folders,
  updates; starts `dantes_inferno.exe`.
- `installer.iss` - the Windows installer (Inno Setup 6).

## Building a release

Requires Visual Studio 2022 Community (MSBuild, .NET Framework 4.0 reference
assemblies) and Inno Setup 6 (`winget install JRSoftware.InnoSetup`).

Build the game first:

```powershell
cmake --preset win-amd64-release -DREXSDK_DIR=thirdparty\rexglue-sdk
cmake --build out\build\win-amd64-release
```

Then:

```powershell
cd launcher
.\package-release.ps1 -Version 0.8.1-beta
```

`package-release.ps1` sets the version in `version.txt` and `installer.iss`,
builds the C# solution, compiles `..\beta-release\DantesInfernoInstaller.exe`
and archives the release's `dantes_inferno.exe` / `.pdb` in
`..\release-symbols\<version>\` (not shipped; used to symbolize crash logs).

The installer bundles `dantes_inferno.exe`, `rexruntime.dll`,
`rexgpu-xenos.dll`, the FidelityFX DLLs, the launcher, `game\default.xexp`
(Title Update 2) when present, and `tools\extract-xiso\extract-xiso.exe`.

## Installer flow

1. Choose the install folder (updates reuse the installed one).
2. Choose the Dante's Inferno Xbox 360 ISO; skipped when `game\default.xex`
   is already there.
3. The ISO is extracted into `<install>\game` with `extract-xiso.exe`; TU2
   goes next to it.
4. Optional desktop shortcut to `DantesInfernoLauncher.exe`.

## Launcher

The launcher keeps its settings in `dantes_inferno.toml` next to
`dantes_inferno.exe` and passes them to the game on the command line
(`DisplayOptions.BuildLaunchArguments`): renderer, resolution, aspect ratio,
frame rate, SMAA, shadow quality, anisotropic filtering, fullscreen, input
backend, button glyphs, language, logging and keybinds.

`renderer_override = "xenos"` in `dantes_inferno.toml` (not in the UI) falls
back to the emulated D3D12 renderer.

## Updates

The launcher reads the releases of `florinp93/hells-gate-recomp` from the
GitHub API, compares the newest tag with the local `version.txt`, and can
download and run that release's `DantesInfernoInstaller.exe`.
