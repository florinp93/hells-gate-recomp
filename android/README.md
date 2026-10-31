# Android port (preview)

arm64-v8a, minSdk 29, Vulkan 1.1. Tested on a Snapdragon 8 Elite (Adreno 830). Uses the SDK's
Xenos Vulkan plugin (`librexgpu-xenos.so`); the Windows-only native renderer is off.

## Build

1. Clone ReXGlue SDK v0.10.0 into `thirdparty/rexglue-sdk`, then apply
   `patches/sdk/rexglue-sdk-v0.10.0.patch` followed by `patches/sdk/rexglue-sdk-android.patch`.
2. Build the host (Linux) SDK and run codegen with your own game files, as in the main README.
   The game must be the title-update image: put `default.xexp` (TU2) next to `default.xex`
   before codegen. Then run `patches/generated/apply_generated_patches.py` and
   `patches/generated/drop_out_of_range_mappings.py`.
3. Cross-compile with NDK r27c:
   `-DCMAKE_TOOLCHAIN_FILE=<ndk>/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a
   -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_shared -DDANTESINFERNO_NATIVE_RENDERER=OFF
   -DDANTESINFERNO_BUILD_LAUNCHER=OFF -DREXGLUE_ENABLE_TRACY=OFF -DREXGLUE_HOST_CLI=<host rexglue>`
   and build the `dantes_inferno` and `rexgpu-xenos` targets.
4. Build the diagnostics helper:
   `<ndk>/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang -shared -fPIC -O1
   -o <dir>/arm64-v8a/libcrashlog.so android/crashlog/crashlog.c -llog -ldl`.
5. Put `libmain.so`, `librexruntime.so`, `librexgpu-xenos.so`, `libSDL3.so`, `libc++_shared.so`
   (and `libcrashlog.so`) in `<dir>/arm64-v8a/`, then in `android/` run
   `gradle assembleDebug -PnativeLibs=<dir> -PsdlJava=<SDL>/android-project/app/src/main/java`.

## Use

- Extract your disc on a PC, copy the folder (with `default.xex` and `default.xexp`) to the phone,
  then tap **Import game folder**.
- **Import saves** copies saves from the PC build's `Documents\dantes_inferno` or Xenia's `content`
  folder (anything containing a `454108CF` folder). **Export saves** backs them up.
- On-screen touch controller (Xbox layout) with a hide/show toggle; physical gamepads also work.
- Launcher options: FPS counter, 30 fps lock, experimental render passes, experimental fast memory
  tracking (known to cause graphics glitches), and a diagnostics profiler.
- Every run writes a log to `Download/HellsGate/` with performance and crash information.

## Known issues

- Heavy scenes run at 20–30 fps; the phone heats up during long sessions.
- Occasional audio crackling (the game's audio thread is CPU-heavy).
