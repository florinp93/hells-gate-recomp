<h1 align="center">Hell's Gate</h1>

<p align="center">
  <strong>Dante's Inferno (Xbox 360), statically recompiled to run natively on PC.</strong>
</p>

<p align="center">
  <img src="assets/fan_artwork.png" alt="Dante's Inferno fan artwork" width="256" />
  <br />
  <em>Fan artwork by <a href="https://www.deviantart.com/pooterman">POOTERMAN</a> (<a href="https://github.com/florinp93/hells-gate-recomp/issues/12">#12</a>)</em>
</p>

<p align="center">
  <a href="https://github.com/florinp93/hells-gate-recomp/releases">
    <img src="https://img.shields.io/github/v/release/florinp93/hells-gate-recomp?include_prereleases&style=for-the-badge&label=Latest%20release&color=B3261E" alt="Latest release" />
  </a>
  <a href="https://ko-fi.com/zerkiller">
    <img src="https://img.shields.io/badge/Ko--Fi-Buy%20me%20a%20coffee-FF5E5B?style=for-the-badge&logo=ko-fi&logoColor=white" alt="Ko-fi" />
  </a>
  <a href="https://discord.gg/mjGfv7ysG8">
    <img src="https://img.shields.io/badge/Discord-Join%20the%20server-5865F2?style=for-the-badge&logo=discord&logoColor=white" alt="Discord" />
  </a>
  <a href="https://github.com/florinp93">
    <img src="https://img.shields.io/badge/Other-Projects-0AB4F5?style=for-the-badge&logo=github&logoColor=white" alt="Other Projects" />
  </a>
</p>

---

Hell's Gate is a PC port of **Dante's Inferno** built with the
[ReXGlue SDK](https://github.com/rexglue/rexglue-sdk). The game's Xbox 360
PowerPC executable is translated ahead of time into portable C++ and compiled
into a native program. There is no emulator and no JIT at runtime.

On top of the recompiled game, Hell's Gate ships its own renderer: the game's
Direct3D layer is replaced by a project-owned implementation on
[DiligentCore](https://github.com/DiligentGraphics/DiligentCore) (Vulkan), so
the game draws at any resolution and aspect ratio without emulating the Xenos
GPU.

> **You need your own copy of the game.** Hell's Gate contains no game files.
> The installer asks for an Xbox 360 ISO of Dante's Inferno that you dumped
> yourself.

## ✨ Features

**Playability**
- Fully playable from start to finish, including saves
- DLC support: drop STFS packages into the DLC folder and they install on launch
- Game language picked from the languages your copy's region contains

**Graphics** (native renderer, default since 0.8.0)
- Any render resolution, detected from your display by default
- Ultrawide and super-ultrawide (16:10, 21:9, 32:9) and the original 4:3
- 60, 120, 180 or 240 FPS, with menus and minigames kept at the correct speed
- SMAA anti-aliasing, anisotropic filtering, higher-resolution shadow maps

**Input**
- Mouse and keyboard with rebindable keys
- Xbox and PlayStation controllers through SDL3 (XInput also available)

**Launcher**
- One-click installer that extracts your ISO
- Settings, keybinds, DLC folder and log toggle in one place
- Built-in updater from GitHub Releases
- Interface in English, Spanish, Portuguese, French and Italian

## 🎮 Getting started

### Windows

1. Download the latest package from
   [Releases](https://github.com/florinp93/hells-gate-recomp/releases).
2. Run `DantesInfernoInstaller.exe`, choose an install folder and point it at
   your Dante's Inferno Xbox 360 ISO.
3. Launch from the desktop shortcut, adjust settings if you like, and press
   **PLAY**.

The launcher writes `dantes_inferno.toml` next to the game and checks for
updates when it opens.

### Linux

- **x86-64:** build from source (see below), or run the Windows build under
  Proton. The Proton path is documented in
  [`docs/linux-cross-compile.md`](docs/linux-cross-compile.md).
- **ARM64:** a Qt launcher and AppImage are available; see
  [`packaging/appimage/README.md`](packaging/appimage/README.md).

A working Vulkan driver is required on every platform.

### DLC

Open the launcher's **DLC** tab, click **Open DLC Folder**, copy your DLC STFS
package files in, then start the game. They are installed automatically on
startup.

## ⚙️ Settings

Everything below is exposed in the launcher. Advanced users can pass the same
options as command-line flags.

| Setting | Flag | Notes |
|---|---|---|
| Resolution | `--dante_resolution=WIDTHxHEIGHT` | Defaults to your display's native resolution |
| Frame rate | `--video_mode_refresh_rate=60` | 60 / 120 / 180 / 240. Use multiples of 60 |
| SMAA | `--dante_smaa=true` | On by default |
| Shadow quality | `--dante_shadow_scale=2` | Shadow map scale, 1–4 |
| Renderer | `--renderer=dante` | `xenos` is the D3D12 fallback (see below) |

If the native renderer misbehaves on your machine, set
`renderer_override` in `dantes_inferno.toml` to fall back to the emulated
Xenos/D3D12 path.

## 🛠️ Building from source

You need your own `default.xex` and game files in `game/` (see
[`game/README.md`](game/README.md)). Requirements: Git, CMake 3.25+, Ninja,
Clang 18+ (Clang 22 on Linux), Python 3 and PowerShell 7 (`pwsh`). Windows
also needs Visual Studio 2022 with the Windows SDK.

The native renderer needs DiligentCore. Clone it **before** running setup so
the project's patches are applied to it:

```bash
git clone --depth 1 --recurse-submodules https://github.com/DiligentGraphics/DiligentCore.git thirdparty/diligent-core
pwsh ./setup.ps1   # clones ReXGlue SDK v0.10.0 and applies the patches in patches/
```

### Windows

```powershell
cmake --preset win-amd64-release -DREXSDK_DIR=thirdparty\rexglue-sdk
cmake --build out\build\win-amd64-release --target dantes_inferno_codegen
python patches\generated\apply_generated_patches.py
cmake --build out\build\win-amd64-release --target dantes_inferno
```

To package the launcher, installer and game into a release folder, run
`launcher\package-release.ps1` (details in
[`launcher/README.md`](launcher/README.md)).

You can also cross-compile the Windows build from Linux with `clang-cl` and
`xwin`; see [`docs/linux-cross-compile.md`](docs/linux-cross-compile.md).

### Linux (Ubuntu, Vulkan)

<details>
<summary>Step-by-step native Linux build</summary>

**1. Install dependencies** (keeps a backup of the manifest, which later steps
overwrite):

```bash
cp dantes_inferno_manifest.toml dantes_inferno_manifest.toml.back
chmod +x ./scripts/install_dantes_min.sh
./scripts/install_dantes_min.sh
```

**2. Set up the SDK** (after cloning DiligentCore as above):

```bash
pwsh ./setup.ps1
```

**3. Configure and build the ReXGlue CLI.** The same configure command is
reused below, so it is kept in a variable:

```bash
CONFIGURE=(cmake -B out/build/linux-release -G Ninja
  -DCMAKE_C_COMPILER=/usr/bin/clang-22
  -DCMAKE_CXX_COMPILER=/usr/bin/clang++-22
  "-DCMAKE_CXX_FLAGS=-stdlib=libstdc++ -I$(pwd)/thirdparty/rexglue-sdk/thirdparty/imgui -mssse3 -mavx2"
  -DREXSDK_DIR=thirdparty/rexglue-sdk)

"${CONFIGURE[@]}"
cmake --build out/build/linux-release --target rexglue
```

**4. Build the Xenos GPU plugin:**

```bash
cmake --build out/build/linux-release --target rexgpu-xenos
cp thirdparty/rexglue-sdk/out/linux-amd64/lib*.so ./out/build/linux-release/
```

**5. Regenerate the SDK-managed project files** (`game/default.xex` must be
present):

```bash
thirdparty/rexglue-sdk/out/linux-amd64/rexglue init \
  --force \
  --project-name dantes_inferno \
  --project-root . \
  --xex-path game/default.xex \
  --game-root game
```

**6. Generate the recompiled C++:**

```bash
"${CONFIGURE[@]}"
cp dantes_inferno_manifest.toml.back dantes_inferno_manifest.toml
cmake --build out/build/linux-release --target dantes_inferno_codegen
```

**7. Apply the generated-code patches:**

```bash
python3 patches/generated/apply_generated_patches.py
```

**8. Build the game:**

```bash
"${CONFIGURE[@]}"
cp dantes_inferno_manifest.toml.back dantes_inferno_manifest.toml
cmake --build out/build/linux-release --target dantes_inferno
rm dantes_inferno_manifest.toml.back
```

The executable is `out/build/linux-release/dantes_inferno`.

**9. Run it** from the repository root:

```bash
./scripts/dantes_inferno_exe.sh
```

</details>

## 🗺️ Status and roadmap

**Current release: 0.8.0-beta**

| Area | Status |
|---|---|
| Recompiled game: boots, saves, fully playable | ✅ Done |
| FMV (VP6) playback fixes | ✅ Done |
| Mouse & keyboard, SDL controller support | ✅ Done |
| Ultrawide and 4:3 | ✅ Done |
| DLC auto-install | ✅ Done |
| High refresh rate (up to 240 FPS) | ✅ Done |
| Native renderer (`--renderer=dante`) | ✅ Default; still being refined |
| SMAA, higher-resolution shadows | ✅ Done |
| Button glyphs (Xbox / PlayStation) | 🔜 Planned |
| Trials of Saint Lucia online server | 🔬 In development ([research](docs/trials_online_server_emulator.md)) |

## 📚 Documentation

| Document | Topic |
|---|---|
| [Native renderer design](docs/NATIVE_RENDERER_DESIGN.md) | How the D3D layer is replaced, milestones |
| [Native D3D map](docs/NATIVE_D3D_MAP.md) | The game's Direct3D entry points |
| [Frame rate](docs/frame_rate.md) | How high refresh rates are made to work |
| [VP6 FMV fix](docs/vp6_fmv_corruption_fix.md) | Video corruption root cause and fix |
| [Ultrawide research](docs/ultrawide_research.md) | Aspect ratio hooks |
| [Linux cross-compile](docs/linux-cross-compile.md) | Building on Linux, running under Proton |
| [ReXGlue notes](docs/rexglue_notes.md) | SDK notes and patches |
| [Trials online emulator](docs/trials_online_server_emulator.md) | DLC online research |

## 🤝 Contributing

Bug reports, pull requests and translations are welcome. The Italian launcher
translation came from the community
([#63](https://github.com/florinp93/hells-gate-recomp/pull/63)), and the ARM64
launcher and AppImage from
[#11](https://github.com/florinp93/hells-gate-recomp/pull/11). Come say hi on
[Discord](https://discord.gg/mjGfv7ysG8).

## 🤖 AI usage disclosure

AI tools were used as an assistant for documentation drafts, research into
APIs and references, and routine Git work such as commit messages. Core
architecture, problem-solving, implementation and final review are done by
the project's human developers.

## Credits

- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk), which makes the
  recompilation possible
- [DiligentCore](https://github.com/DiligentGraphics/DiligentCore), the
  graphics layer under the native renderer
- [POOTERMAN](https://www.deviantart.com/pooterman) for the fan artwork
- Everyone who has tested, reported bugs and contributed

## Legal

Hell's Gate is an unofficial fan project and is not affiliated with or
endorsed by Electronic Arts, Visceral Games or Microsoft. *Dante's Inferno* is
a trademark of its respective owners. This repository contains no game code
or assets; you must provide your own legally obtained copy of the game.
