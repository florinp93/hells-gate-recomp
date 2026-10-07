# Native Renderer Design: D3D-Layer Replacement

**Status:** M0 complete (2026-10-07). Supersedes the scene-data approach in
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
REX_HOOK_RAW overrides  (src/native_renderer/d3d_hooks.cpp)
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

### M1: Confirm the map (needs one traced run)
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

### M3: Native frame skeleton
- `--renderer=dante`: Xenos command processor no longer receives draws.
- Native render targets + `Clear` + `Resolve` + `Swap` present through
  DiligentCore. Draws are counted but not issued (screen shows clears/resolves).
- Fences/sync satisfied natively; game stays stable and paced.
- Exit: game boots to menu and runs gameplay without hangs (visuals black).

### M4: Shaders, buffers, textures
- Shader translation reusing the SDK's SPIR-V translator with a project-owned
  binding layout (first step), compiled ahead of time from the shipped cache.
  Later option: an XenosRecomp-style HLSL translator for readable,
  per-shader-editable output.
- Vertex/index buffers from fetch constants; textures untiled once per
  content change.
- Exit: main menu and the first level render correctly natively.

### M5: Parity
- All 19 passes (shadow CSM, G-buffer, deferred lights, forward, VFX, Alchemy
  post), FMV (VP6), UI, `BeginVertices` paths.
- Exit: full playthrough checkpoints match the Xenos path visually.

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
