# Repository index

Every file tracked on `main`, what uses it and whether it stays. "Build" is the
CMake game build (`tools\build_and_run.bat`), "Release" is
`launcher\package-release.ps1` with `installer.iss`, "Runtime" is the shipped
game or launcher, "Docs" is a workflow described in the README or `docs/`.

## Removed

| File | What it was | Why it went |
|---|---|---|
| `launcher/DantesInfernoInstaller/*` (6 files) | WPF installer | Built by the solution but never shipped: the release installer is `installer.iss` (Inno Setup). |
| `launcher/DantesInfernoUninstaller/*` (5 files) | WPF uninstaller | Same; Inno Setup writes its own uninstaller. |
| Installer strings in `LauncherLocalizer.cs` (43 keys x 5 languages) | UI text of the WPF installer | Only the removed installer read them. |
| `GitHubUpdater.SetLocalVersion`, `ReleaseInfo.Prerelease`, `SemanticVersion.BuildMetadata` | Updater helpers | No caller left. |
| `tools/pipeline_storage_info.py` | Prints a `pipelines.bin` summary | Not referenced by any build, release step or doc; `pipeline_storage_merge.py` is the one the release uses. |
| `metadata/icon.ico`, `metadata/.gitkeep` | Game exe icon | Byte-identical to `launcher/icon.ico`; `src/dantes_inferno.rc` now points there. |
| `generated/.gitkeep` | Keeps `generated/` in git | `generated/rexglue.cmake` is tracked, so the folder exists anyway. |

## Kept

### Root

| File | What it is | Used by | Verdict |
|---|---|---|---|
| `.clang-format` | C++ style | Editors, clang-format | Keep |
| `.gitattributes` | Line endings, binary types | Git | Keep |
| `.github/FUNDING.yml` | Ko-fi sponsor button | GitHub | Keep |
| `.gitignore` | Ignore rules | Git | Keep (dropped the entries of removed files) |
| `.recomp.json` | Listing metadata for recomp.fyi | External index | Keep |
| `CMakeLists.txt` | Game, renderer and Qt launcher build | Build | Keep |
| `CMakePresets.json` | `win-amd64-release` and other presets | Build, README | Keep |
| `README.md` | Project page | Docs | Keep |
| `assets/fan_artwork.png` | README artwork | README | Keep |
| `dantes_inferno_manifest.toml` | ReXGlue manifest (hooks, TU2 entry points) | Build (codegen) | Keep, SDK-managed |
| `setup.ps1`, `setup.sh` | Clone the SDK / DiligentCore, apply patches | README | Keep |
| `game/README.md` | Where the game files go | README | Keep |
| `generated/rexglue.cmake` | SDK build glue | Build | Keep, SDK-managed |
| `cmake/toolchains/windows-clang-cl-xwin.cmake` | Cross-compile toolchain | `docs/linux-cross-compile.md` | Keep |

### Game sources (`src/`)

| File | What it is | Used by | Verdict |
|---|---|---|---|
| `main.cpp` | App entry point | Build | Keep, SDK-managed |
| `dantes_inferno_app.h` | App hooks: paths, keybinds, overlays, DLC auto-install, debug binds | Build | Keep (keybind defaults and bind list made tables) |
| `dantes_inferno_hooks.h` | Save-system setjmp/longjmp, ultrawide aspect hook | Generated code | Keep |
| `dantes_inferno.rc` | Exe icon | Build (Windows) | Keep (icon path changed) |
| `crash_log.h` | Crash handler and game file report | Runtime | Keep |
| `damage_hooks.cpp` | Damage trace, dev cheats | Build | Keep |
| `frame_rate_hooks.cpp` | 60 Hz UI at high refresh rates | Build | Keep |
| `guest_memory.h` | Big-endian guest memory helpers | Build | Keep |
| `path_util.h` | UTF-8 paths, wide `fopen` | Build | Keep |
| `session_info.h` | Session header in the log | Runtime | Keep |
| `timer_resolution.*` | 1 ms Windows timer | Runtime | Keep |
| `user_paths.*` | Saves/cache location, portable mode | Runtime | Keep |
| `overlay/*` (10 files) | Achievements UI and sound, FPS overlay, shader progress, styling | Runtime | Keep |

### Renderer (`src/renderer/`)

| File | What it is | Used by | Verdict |
|---|---|---|---|
| `renderer.*`, `draw_renderer.*`, `texture_cache.*`, `shader_cache.*`, `guest_gpu_state.*`, `vulkan_device.*` | Native renderer core | Build | Keep |
| `d3d_hooks.cpp` | Hooks on the game's D3D calls | Build | Keep |
| `state_verifier.*` | F9 GPU state verifier | Debug workflow (CLAUDE.md) | Keep |
| `pipeline_storage.*` | Recorded shader/pipeline list | Runtime, release (`dante_pipelines.bin`) | Keep |
| `button_glyphs.*` | PlayStation/keyboard button prompts | Runtime | Keep |
| `smaa_pass.*` | SMAA | Runtime | Keep |
| `ui_overlay.*` | ImGui overlay drawing | Runtime | Keep |
| `png_dump.*` | F8 frame dump | Debug workflow | Keep |
| `renderdoc_capture.*`, `renderdoc_app.h` | Insert-key RenderDoc capture; RenderDoc API header 1.1.2 (the SDK copy is 1.0) | Debug workflow | Keep |
| `render_stats.h`, `renderer_config.*`, `swizzle.h`, `thread_cpu.*` | Counters, renderer cvars, texture untiling, per-thread CPU usage for the performance log | Build | Keep |
| `dantes_inferno_native_app.h`, `dantes_inferno_native_main.cpp` | `dantes_inferno_native` exe (SDK Vulkan backend, `--renderer=native`) | `tools/native_run.bat`, NATIVE_RENDERER_DESIGN.md | Keep as the reference renderer. Candidate for removal if you no longer compare against it. |

### Third party

| File | What it is | Used by | Verdict |
|---|---|---|---|
| `thirdparty/smaa/SMAA.hlsl`, `Textures/AreaTex.h`, `Textures/SearchTex.h` | SMAA shader and lookup textures | `smaa_pass.cpp` | Keep |
| `thirdparty/smaa/LICENSE.txt`, `README.md` | SMAA license and upstream readme | Attribution | Keep |

### Patches

| File | What it is | Used by | Verdict |
|---|---|---|---|
| `patches/sdk/rexglue-sdk-v0.10.0.patch`, `apply_sdk_patches.ps1` | SDK changes | `setup.ps1`, `setup.sh` | Keep (both apply scripts shortened; they popped the location twice on early exit) |
| `patches/diligent/diligent-core.patch`, `apply_diligent_patches.ps1` | DiligentCore changes | `setup.ps1`, `setup.sh` | Keep |
| `patches/generated/apply_generated_patches.py` | Post-codegen fixes (fibers, unresolved calls) | Build steps in README | Keep |

### Windows launcher and release (`launcher/`)

| File | What it is | Used by | Verdict |
|---|---|---|---|
| `DantesInferno.sln` | Shared + Launcher projects | Release | Keep (installer projects removed) |
| `DantesInferno.Shared/*` | Settings, launch arguments, ISO check, DLC install, updater, UI text | Launcher | Keep (updater and XEX parser shortened) |
| `DantesInfernoLauncher/*` | The launcher | Release, runtime | Keep |
| `DantesInfernoLauncher/version.txt` | Release version | Release, updater | Keep |
| `Theme.xaml`, `icon.ico`, `banner.jpg` | Launcher look, icon (also the exe icon), banner | Launcher, installer | Keep |
| `installer.iss` | Inno Setup installer | Release | Keep |
| `package-release.ps1` | Builds the release | Release | Keep (shortened version and status handling) |
| `README.md` | Release how-to | Docs | Keep |

### Linux

| File | What it is | Used by | Verdict |
|---|---|---|---|
| `launcher-linux/*` | Qt launcher and its test | CMake on Linux, AppImage | Keep |
| `packaging/appimage/*` | ARM64 AppImage recipe | README | Keep |
| `scripts/install_dantes_min.sh`, `install_opensuse.sh` | Build dependencies | README | Keep |
| `scripts/dantes_inferno_exe.sh` | Runs the Linux build | README | Keep (it never exported `LD_LIBRARY_PATH`; fixed) |
| `scripts/run-linux.sh` | Runs the Windows build under Proton | `docs/linux-cross-compile.md` | Keep |

### Tools

| File | What it is | Used by | Verdict |
|---|---|---|---|
| `tools/build_and_run.bat` | Build and launch | Dev workflow | Keep |
| `tools/native_run.bat` | Build and launch `dantes_inferno_native` | Dev workflow | Keep with the native target |
| `tools/pipeline_storage_merge.py` | Merges recorded pipeline lists into `dante_pipelines.bin` | Release (`installer.iss`) | Keep |

### Docs

| File | Topic | Linked from | Verdict |
|---|---|---|---|
| `docs/NATIVE_RENDERER_DESIGN.md` | Native renderer design | README, CLAUDE.md | Keep |
| `docs/NATIVE_D3D_MAP.md` | D3D addresses and register layout | README, CLAUDE.md | Keep |
| `docs/frame_rate.md` | High refresh rate | README | Keep |
| `docs/vp6_fmv_corruption_fix.md` | VMX builder fixes | README, AGENTS.md | Keep |
| `docs/linux-cross-compile.md` | Linux builds, Proton | README | Keep |
| `docs/rexglue_notes.md` | SDK reference | README | Keep |
| `docs/trials_online_server_emulator.md` | Trials online research | README | Keep |
| `docs/REPO_INDEX.md` | This file | | Keep |
