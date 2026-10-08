# Native Renderer Design: D3D-Layer Replacement

**Status:** M0-M2 complete, M3 designed (2026-10-07). Supersedes the scene-data approach in
`tools/research/dante_re/TRANSITION/NATIVE_RENDERER_PLAN.md` (RE-030 judged it
impractical) and the "replace Xenos piece by piece" plan in
`docs/HYBRID_RENDERER_PLAN.md`.

## Goal

Render the game without emulating the Xenos GPU: no PM4 ring, no command
processor, no EDRAM, no runtime ucode translation stalls. The recompiled game
keeps running unchanged; only its **statically linked D3D library** is
replaced by a project-owned implementation on DiligentCore (Vulkan, D3D12
optional). This is the model Sonic Unleashed Recompiled used.

`--renderer=xenos` (and the current in-process `--renderer=native`) stay as
fallbacks. The new path gets its own value, `--renderer=dante`, until it
reaches parity.

## Why this layer

| Layer | Problem |
|-------|---------|
| Xenos registers / PM4 (today) | Emulation: EDRAM, resolves through emulated memory, 720p-shaped state, translation stutter. |
| Scene data (RenderWare/EARS structs) | Needs RE of the whole engine; RE-030 found it blocked. |
| **D3D API (this design)** | Small, well-defined surface (~40 entry points). The device already holds decoded state as a register shadow. The game never sees the difference. |

Map of the D3D layer: `docs/NATIVE_D3D_MAP.md`.

## Architecture

```
recompiled game (unchanged)
   │  calls D3DDevice_* (sub_827Cxxxx … sub_8291Dxxx)
   ▼
REX_HOOK_RAW overrides  (src/native_renderer/d3d_trace.cpp)
   │  renderer != dante → __imp__sub_XXXX (original, Xenos path)
   │  renderer == dante ↓
   ▼
DanteDevice  (project-owned, host side)
   ├─ StateReader     reads the guest device's register shadow at draw time
   │                  (+1152 fetch, +1920/+6016 constants, +10368.. regs)
   ├─ ShaderCache     Xenos ucode → SPIR-V, keyed by ucode hash, built AOT
   │                  from the bundled .xsh cache; project binding layout
   ├─ TextureCache    fetch constant → untiled native texture, keyed by guest
   │                  address + format + size; invalidated on CPU writes
   ├─ BufferCache     vertex/index data → native buffers (endian swap on upload)
   ├─ RenderTargets   native RTs keyed by surface object / EDRAM base;
   │                  any resolution, any aspect, no EDRAM
   ├─ Resolve         native copy RT → texture registered at the destination
   │                  guest address (no round trip through guest memory unless
   │                  the CPU reads it)
   └─ Present         Swap → DiligentCore swapchain + post (CAS/FSR/TAA)
   ▼
DiligentCore → Vulkan / D3D12
```

Things that must keep working when the ring is gone:

- **Fences / GPU sync**: the game waits on D3D fences. Hooks complete them
  against the native GPU timeline (or immediately at first).
- **Vblank interrupt**: the SDK keeps calling the guest graphics interrupt
  callback; frame pacing stays as today.
- **CPU reads of GPU output** (screenshots, save thumbnails, queries): resolve
  targets the CPU reads must be written back to guest memory on demand.

## Milestones

Each milestone ends with a build the user runs; the Xenos path stays default.

### M0: Map the D3D layer (DONE, 2026-10-07)
- Function override mechanism confirmed (weak aliases, `REX_HOOK_RAW`).
- D3D layer located and mapped; device register-shadow layout derived.
- Diagnostic hooks added: `src/native_renderer/d3d_trace.cpp`
  (`--d3d_trace_frames=N` or **F8** in game).
- GPU pass tracer saved into the SDK patch.

### M1: Confirm the map (DONE, 2026-10-07)
- Run the game to a gameplay scene, press F8 (and once on the main menu).
- Use the `D3D-TRACE` log to settle argument orders and the LIKELY/GUESS
  entries in the map; find the fence API and shader/texture object layouts.
- Exit: every function in the hook list has a confirmed name and signature.

### M2: Shadow observer (no rendering change)
- Hooks on the draw functions build a `NativeDrawState` (render targets,
  depth/stencil/blend/raster state, shaders, fetch constants, constants) from
  the guest device **before** calling the original.
- Cross-check against the Xenos command processor's register file at the same
  draw (diagnostic cvar). Disagreement = decoding bug.
- Exit: one full in-game frame decodes with zero mismatches.
- **Done 2026-10-07** (log 323): menu, gameplay and pause frames, all draws
  match the Xenos register file on every shader-read register. Rules in
  `docs/NATIVE_D3D_MAP.md`, "Effective GPU state at a draw". F9 /
  `--native_observe_frames=N`.

### M3: Native frame skeleton (DONE except optional M3.5, 2026-10-07)

**Goal:** `--renderer=dante` owns render targets, `Clear`, `Resolve` and
`Swap` and presents through DiligentCore. Draws are counted, not issued, so
the screen shows clears and resolves only. The game must stay stable and
paced through menu, loading and gameplay.

#### Decision: keep the ring as the sync engine (M3), remove it later

The game waits on the GPU in exactly one way: a fence F is complete when the
word at `*[dev+10896]` has reached F (`0x827D3060`; `dev+10908` is the next
fence). The command processor (CP) advances that word when it executes the
`EVENT_WRITE_SHD` packets D3D puts in the ring, and the ring kick
(`0x827D3F10`) itself waits on fences when the ring is full. Vblank
interrupts, `VdSwap` and the occlusion-query result writes all go through the
same CP.

So M3 does **not** touch the D3D layer's ring, fences or swap:

- Every hooked D3D function still calls the original. The ring is filled
  and consumed exactly as today; fences, interrupts and pacing are unchanged.
- An SDK switch turns the CP into a **sync-only CP**: it keeps executing
  packets (register writes, memory/fence writes, interrupts, swaps) but skips
  host rendering (`IssueDraw`, which also covers clears and resolves, which
  are draws on Xenos). With `occlusion_query_enable=false` the CP writes the
  existing fake "visible" sample counts (`query_occlusion_fake_sample_count`).
- The native renderer works **after** each original call from the effective
  GPU state proven in M2 (`docs/NATIVE_D3D_MAP.md`, "Effective GPU state at a
  draw").

Removing the ring entirely (fences completed against the native GPU
timeline, kick turned into a no-op) becomes its own late milestone (M5b)
once the native path renders everything. It is an optimization, not a
prerequisite.

#### Components

```
d3d hooks (d3d_trace.cpp)  --after original-->  DanteDevice (new, render thread)
                                                   |-- GpuState     M2 state builder, shared with the observer
                                                   |-- EdramModel   host RTs keyed by EDRAM base/pitch/format/MSAA
                                                   |-- Resolver     RB_COPY_* -> SDK draw_util::GetResolveInfo -> copy
                                                   |-- TextureRegistry  guest address -> native texture (resolve outputs)
                                                   `-- Presenter    Swap front buffer -> Diligent swapchain on the game window
```

- **GpuState**: the M2 observer's expected-state builder becomes a library
  used by both the observer and `DanteDevice`. It produces a full
  `rex::graphics::RegisterFile`, so SDK helpers (`draw_util`, texture info,
  format tables) can be reused without hand decoding.
- **EdramModel**: a host render target per (EDRAM base tile, pitch,
  format, MSAA), sized from the surface (`surface+0x18/+0x1C/+0x2C`).
  Static surface objects are re-initialized every frame, so binding is by
  register values, never by object pointer. Host RTs are created at
  `resolution_scale` from day one. Overlapping EDRAM ranges written with a
  different key without a clear in between are logged (`EDRAM-ALIAS`) for M5.
- **Clear** (`0x827EDC18`, after the original): clears the bound RT0-3 /
  depth on flags `TARGET0..3` (bits 0-3), `ZBUFFER` (`0x10`), `STENCIL`
  (`0x20`) with the given color/Z/stencil over the rect.
- **Resolve** (`0x827E5B18`, also reached from `EndTiling`): after the
  original, the `RB_COPY_*` registers in the GpuState describe source,
  destination address/format/pitch and clear. `GetResolveInfo` decodes them;
  the Resolver copies the host RT into the native texture registered at the
  destination guest address (no write-back to guest memory), then applies
  the post-resolve clear if requested.
- **Tiling**: the game always uses one tile, so `BeginTiling`/`EndTiling`
  are a normal pass plus the resolve above; `SetPredication` is ignored.
- **Swap** (`0x827D4EE0`): the front buffer texture (`r4`, 1280x720 at
  `0xAA001000`/`0xAA39A000`) is looked up in the TextureRegistry and
  presented. Presentation must not block the render thread; the game's own
  vblank pacing stays in charge.
- **Presenter**: reuse `native_presenter.cpp` (DiligentCore swapchain that
  detaches the SDK presenter from the window). ImGui overlays (FPS, settings,
  achievements) are not shown on this path until M6.
- **Draws**: counted per frame and logged with the pass (RT key), not issued.

#### Steps (each ends with a build the user runs)

1. **M3.1 Sync-only CP.** SDK cvar `gpu_host_rendering` (default true).
   `--renderer=dante` sets it false plus `occlusion_query_enable=false`; the
   SDK presenter keeps presenting (black or stale image). Check: menu,
   loading, gameplay, pause, FMV playback and a save all work without hangs;
   frame pacing unchanged. This proves the sync contract before any native
   work. Also add a memexport flag to the M2 usage struct and log any
   memexporting shader (memexport draws would lose their output when
   skipped).
2. **M3.2 GpuState refactor.** Move the M2 state builder into
   `gpu_state.{h,cpp}`; the observer uses it unchanged (F9 must stay clean).
3. **M3.3 Native presenter + Swap.** DiligentCore swapchain on the game
   window; Swap presents a debug pattern plus the frame counter. Check:
   window, resize, fullscreen, alt-tab, no pacing change.
4. **M3.4 EDRAM model + Clear + Resolve.** Host RTs, clears, resolves into
   the TextureRegistry, Swap presents the resolved front buffer. Check: the
   clear colors of each pass reach the screen (mostly black); `NATIVE-RT`
   log lists the per-frame passes, matching the 19-pass shape from the GPU
   pass trace.
5. **M3.5 A/B switch (optional).** A key toggles between the native image
   and the Xenos image (with host rendering on) to compare passes; reuses the
   step-0 GPU interop path. Useful from M4 onwards.

**Exit:** `--renderer=dante` boots to the menu and plays through gameplay,
pause and FMV without hangs or pacing changes; resolves and swaps land in
the right native textures (verified by the `NATIVE-RT` pass log).

#### Status (2026-10-07)

- **M3.1 done** (log 325): `gpu_host_rendering=false` (SDK) and
  `occlusion_query_enable=false` under `--renderer=dante`; F10 toggles host
  rendering at runtime. Steady 60 fps black, saves work, 15 on/off toggles
  without issue. memexport: 0 draws (log 326).
- **M3.2 done** (log 326): `src/native_renderer/gpu_state.{h,cpp}`; F9 still
  975/975 clean.
- **M3.3 done** (log 326): `src/native_renderer/dante_device.{h,cpp}` owns a
  DiligentCore swapchain on the game window; window handling verified.
- **M3.4 done** (log 328): host RTs per (EDRAM base, pitch, format, depth),
  `Clear`, `Resolve` (from the call's own arguments; flags `&7` source,
  `0x100`/`0x200` clear afterwards) and `Swap` presenting the resolved front
  buffer. The frame flows as: half-res 640x360 pass -> full-res depth ->
  960x960 shadow depth -> 80x16 reduction -> final 1280x720 pass -> resolve
  to `AAE77000` and the front buffer (`AA39A000`/`AA001000`) -> present.
  F8 also logs the per-frame `NATIVE-RT` sequence.
- Found: the game resolves different sizes/formats to the **same guest
  address** within a frame (`AAE77000`, `AA733000` as 640x360 and
  1280x720), so native resolve targets are keyed by (address, width,
  height, format).
- Depth resolves to `k_24_8` textures use a shader copy into R32_FLOAT (M4).
- M3.5 (A/B switch) replaced by side-by-side screenshots against the Xenos
  plugin and the SDK Vulkan backend (`tools/native_run.bat`).

#### Open items to settle during M3

- Resolve flag bits beyond 0 / 4 / `0x14` and the clear-after-resolve
  encoding (read from `0x827E5B18` and `RB_COPY_CONTROL`).
- Whether anything reads resolve output on the CPU (save thumbnails,
  screenshots). The Xenos path runs with `readback_resolve=none`, so probably
  not; log page reads of resolve destinations if in doubt.
- Whether any shader uses memexport (step 1 diagnostic).
- Native work runs on the game's render thread inside the hooks; measure the
  cost and move submission to a worker if it shows up in frame time.

### M4: Shaders, buffers, textures (DONE 2026-10-08, one known issue)

**Goal:** draws render natively under `--renderer=dante`; main menu and the
first level look like the Xenos path.

#### Decision: reuse the SDK SPIR-V translator, implement its contract natively

The SDK's `SpirvShaderTranslator` (already compiled into the exe via
`dante_gpu`) turns Xenos ucode into SPIR-V and is proven on this game. Its
output expects a fixed resource contract, which `DanteDevice` provides
through DiligentCore (resources are named in the SPIR-V, so Diligent binds
them by name):

| Set | Contents (translator names) | Native source |
|---|---|---|
| 0 | `xe_shared_memory`: 512 MB SSBO = guest physical memory | Native buffer; vertex/index ranges uploaded raw (big-endian; the shader swaps) |
| 1 | `xe_uniform_system_constants`, float VS/PS, bool/loop, fetch constants | `gpu::State` (M2-exact) + system constants computed per draw |
| 2/3 | `xe_texture{N}_{dim}_{s/u}` + samplers (VS/PS) | Native textures: untiled guest textures or M3 resolve targets |

Host render targets are used (no FSI/EDRAM-in-shader), so the `edram_*`
system constants stay zero. Logic to reuse from the SDK Vulkan backend rather
than re-derive: shader modification selection (`vulkan/pipeline_cache.cpp`),
system constants (`UpdateSystemConstantValues`), viewport
(`draw_util::GetHostViewportInfo`), texture layout/untiling
(`texture_info`/`texture_util`). A readable XenosRecomp-style HLSL translator
stays a later option.

Ucode sources (`docs/NATIVE_D3D_MAP.md`): PS = `PS[+0x18] + blk[+40]`, size
`blk[+44]`; VS = `VS[+0x20] + blk[+872]` of the bound variant. M4 can also
take the variant the CP actually loaded (IM_LOAD address) until the variant
choice is decoded.

#### Steps (each ends with a build the user runs)

1. **M4.1 Translation only.** At each draw, translate the bound VS/PS (cache
   by ucode hash + modification), create Diligent shaders/PSOs, draw
   nothing. Log `NATIVE-SH` counts and failures. Exit: zero failures in
   menu and gameplay.
2. **M4.2 Geometry.** Shared-memory buffer (uploads of the draw's vertex and
   index ranges, skipped when a content hash is unchanged), constant
   buffers, system constants (NDC transform, vertex index endian/base),
   basic PSO state, draw into the host RTs. Quad lists / fans expanded to
   triangle lists on the CPU. Exit: untextured geometry visible in place.
3. **M4.3 Textures.** Fetch constant -> native texture: resolve targets by
   (address, w, h, format) first, otherwise untile + upload (DXT/8888/...)
   cached by address + content hash; samplers from fetch dwords 3/4.
   Resolve conversions (depth -> sampled depth/shadow textures) via a shader
   copy. Exit: textured menu and level.
4. **M4.4 Render state parity.** Blend, depth/stencil, cull, color mask,
   alpha test, color exponent bias, gamma; A/B toggle (deferred M3.5) to
   compare against Xenos. Exit: menu and first level match the Xenos path.

#### Status (2026-10-08): done, one known issue

Menus, FMV-backed menu background, loading screens, HUD and gameplay render
through renderer=dante at 60 fps and match Xenos screenshots (colour grade,
lighting, shadows, particles, post). The M2 observer runs under
renderer=dante too: every compared draw matches the command processor in
registers, VS/PS ucode hash and index buffer.

What it took beyond the plan above:
- VS microcode from the program flush's `IM_LOAD` / `IM_LOAD_IMMEDIATE`
  (D3D patches fetches per declaration and copies busy variants to the ring).
- Stale literal constants persist across draws (`gpu_state`).
- Resolve `copy_dest_swap` tracked per resolve target and folded into view
  swizzles; the front buffer is presented with its own fetch swizzle.
- Shared-memory uploads: overlapping ranges invalidate each other; static
  ranges are hashed once per frame, dynamic ones per draw.
- Textures: SDK guest layout loader for all mip levels (packed tails),
  stacked 2D, cube and 3D textures, typeless textures with SNORM views for
  signed fetches, zero for missing / unsupported (as the SDK).
- Render state: cull (front face clockwise if `face`), depth bias, primitive
  reset for strips only.

Known issue (first M5 item): part of Dante's model (between the legs, the
dark speckled look from behind) still differs from Xenos and from the SDK
Vulkan backend (`tools/native_run.bat`, renderer=native). Verified identical
between renderer=dante and the SDK Vulkan backend: registers, shaders (SPIR-V
bodies), constant buffers, index buffers, vertex data, textures. Details in
the progress log.

#### Risks

- Per-draw uploads cost CPU on the game's render thread; start with content
  hashes, move to guest-memory write tracking if needed.
- The translator's texture binding assumptions (2D textures as arrays,
  swizzles in system constants) must be matched exactly; mismatches show up
  as validation errors, which Diligent logs.
- Primitive restart: Vulkan only restarts at 0xFFFF/0xFFFFFFFF; check
  `VGT_MULTI_PRIM_IB_RESET_INDX` (strips are used for all world geometry).

### M5: Parity
- All 19 passes (shadow CSM, G-buffer, deferred lights, forward, VFX, Alchemy
  post), FMV (VP6), UI, `BeginVertices` paths.
- Exit: full playthrough checkpoints match the Xenos path visually.

### M5b: Drop the command processor
- Ring kick (`0x827D3F10`) becomes a no-op; `InsertFence`/`BlockOnFence`
  complete against the native GPU timeline by writing `*[dev+10896]`;
  occlusion queries answered natively; `VdSwap` handled without PM4.
- Exit: no PM4 parsing at all, same behaviour as M5.

### M6: Native-only features
- True internal resolution (RTs we own), ultrawide without letterboxing,
  TAA with game motion vectors, FSR 2/3, high-refresh decoupling, ImGui
  overlays on the native presenter.

## Risks / decisions to watch

| Risk | Mitigation |
|------|-----------|
| Game code inlines some D3D setters | Read state from the device shadow at draw time instead of hooking setters. |
| Resolve-to-texture feedback (textures sampled from resolve targets) | Texture cache looks up native resolve targets by guest address first. |
| CPU reads of rendered data | Lazy write-back to guest memory when a page is read (watch) or on Lock. |
| memexport / viz queries | Detect in shader analysis; viz queries report visible as Xenos path does today. |
| Translator binding model tied to Xenia RT cache | M4 starts with a minimal project binding layout; translator features for EDRAM (FSI) are not used. |
| Cannot build/run from the agent's sandbox | Small milestones with trace/diagnostic cvars; the user runs each build. |
| Skipped Xenos draws hide side effects (memexport, CPU-read resolves) | M3.1 diagnostics; keep `gpu_host_rendering=true` as an A/B fallback. |
| Native work on the game's render thread costs frame time | Measure in M3.4; move submission to a worker thread if needed. |
