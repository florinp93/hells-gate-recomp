# Android port (preview, untested on device)

arm64-v8a, minSdk 29, Vulkan 1.1. Uses the SDK's Xenos Vulkan plugin
(`librexgpu-xenos.so`); the Windows-only native renderer is OFF.

Build:
1. Apply `patches/sdk/rexglue-sdk-v0.10.0.patch`, then `patches/sdk/rexglue-sdk-android.patch`.
2. Run codegen on the host with your own `default.xex` (README steps 5-7).
3. Cross-compile with NDK r27c: `-DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29
   -DANDROID_STL=c++_shared -DDANTESINFERNO_NATIVE_RENDERER=OFF -DDANTESINFERNO_BUILD_LAUNCHER=OFF
   -DREXGLUE_HOST_CLI=<host rexglue>`.
4. Put libmain, librexruntime, librexgpu-xenos, libSDL3, libc++_shared in `<dir>/arm64-v8a/` and run
   `gradle assembleDebug -PnativeLibs=<dir> -PsdlJava=<SDL>/android-project/app/src/main/java`.

Use: extract your game disc on a PC, copy the folder (with default.xex) to the phone, tap
"Import game folder" in the app. Physical gamepad only; no touch controls yet.
Note: hooks for 3 TU2-only functions are stubbed because the base-image XEX lacks them.
