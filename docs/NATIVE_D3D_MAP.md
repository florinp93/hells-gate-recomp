# Xbox D3D Layer Map (TU2 image)

Static identification of Dante's Inferno's **statically linked XDK Direct3D
library** inside the recompiled image. This is the interception layer for the
truly native renderer (see `docs/NATIVE_RENDERER_DESIGN.md`).

Source of truth for addresses: the live TU2 guest image
(`logs/guest_image.bin`, dumped with `--dlc_dump_image=true`) and the PPC
disassembly kept as comments in `generated/default/*.cpp`.

Confidence tags: **CONFIRMED** (unambiguous from code), **LIKELY** (strong
structural evidence, needs the runtime trace), **GUESS**.

> Correction to earlier research: RE-022 / RE-030 assumed D3D calls were XEX
> imports resolved at runtime. They are not. Xbox 360 D3D is a static XDK
> library and is fully present in the recompiled code.

## Where it lives

| Range | Contents |
|-------|----------|
| `0x827C8000`–`0x827EFFFF` | XDK D3D library (render-state setters, resources, ring/command buffer, resolve, swap, device init). ~600 functions. |
| `0x8291C000`–`0x8291E000` | TU2-relocated D3D draw paths (`DrawIndexedVertices`, `DrawVertices`, `BeginVertices`…). |
| `0x828E2000`–`0x828E3000` | A few TU2-relocated D3D helpers (write VS constants 139–141). |
| `0x824D4000`–`0x824DC000` | EA engine render wrapper layer (the main caller of D3D: shaders, RTs, resolve, swap). Not D3D itself. |

## Overriding a recompiled function

Every generated function is emitted as
`__attribute__((weak, alias("__imp__sub_XXXXXXXX"))) sub_XXXXXXXX` (SDK
`resources/templates/codegen/pch_h.inja`). A strong definition in project code
replaces it for **both direct calls and the indirect-call table**
(`dantes_inferno_register.cpp` registers `sub_XXXXXXXX`). The original body
stays callable as `__imp__sub_XXXXXXXX`.

```cpp
#include <rex/hook.h>
REX_EXTERN(__imp__sub_8291D328);
REX_HOOK_RAW(sub_8291D328) {          // D3DDevice_DrawIndexedVertices
  if (!NativeRendererActive()) return __imp__sub_8291D328(ctx, base);
  ...native path...
}
```

Hooks must be compiled into the executable targets (see `CMakeLists.txt`,
`DANTESINFERNO_SOURCES`). The SDK's own `rexcrt_*` CRT hooks use the same
mechanism, so no codegen change is needed. Optionally, functions can be given
readable symbol names via `[entrypoint.functions.0xADDR] name = "..."` in the
manifest (requires a codegen regen).

## D3D device object layout (r3 in every D3DDevice_* call)

The device keeps a **shadow of the Xenos register file** plus 64-bit dirty
masks; nothing is written to the ring until a draw/resolve flushes dirty
groups. Layout derived from the flush code in `DrawIndexedVertices`
(`sub_8291D328`) and the flushers `sub_827E92E0` (type-0 register runs),
`sub_827E9520` (fetch constants), `sub_827E9678` (ALU constants):

| Offset | Contents | Evidence |
|--------|----------|----------|
| `+0` u64 | dirty mask: VS float constants (flushed to reg `0x4000`) | CONFIRMED |
| `+8` u64 | dirty mask: PS float constants (reg `0x4400`) | CONFIRMED |
| `+16` u64 | dirty mask: render-state groups `0x2000/0x2100/0x2180/0x2200`, fetch/shader bits | CONFIRMED |
| `+24` u64 | low 32 bits: dirty fetch-constant slots; upper: group `0x2280` | CONFIRMED |
| `+32` u64 | dirty: groups `0x2300`, `0x2380`, misc, bool/loop constants (bit 56) | CONFIRMED |
| `+40` u64 | mask used before `sub_827E8F48` (shader-dependent state) | LIKELY |
| `+48` / `+52` / `+56` | command-ring write pointer / limit / kick threshold | CONFIRMED |
| `+1152` | **fetch constants**: 32 slots × 24 bytes (6 dwords), reg `0x4800` | CONFIRMED |
| `+1920` | **VS float constants** 256 × float4 | CONFIRMED |
| `+6016` | **PS float constants** 256 × float4 | CONFIRMED |
| `+10112` | bool + loop constants (reg `0x4900`) | CONFIRMED |
| `+10368` | regs `0x2000`–`0x200F` (`RB_SURFACE_INFO`, `RB_COLOR_INFO`, `RB_DEPTH_INFO`, …); exactly 16, `+10432..+10443` is bookkeeping (e.g. `0x02D00500` = 720/1280) | CONFIRMED (M2) |
| `+10444` | regs `0x2100`–`0x2114` (`VGT_*`, `RB_COLOR_MASK`, `RB_STENCILREFMASK`, …) | CONFIRMED |
| `+10528` | regs `0x2180`–`0x2184` (`SQ_PROGRAM_CNTL`, …) | CONFIRMED |
| `+10548` | regs `0x2200`–`0x220B` (`RB_DEPTHCONTROL`, `RB_BLENDCONTROL*`, `RB_COLORCONTROL`, …) | CONFIRMED |
| `+10596` | regs `0x2280`… (`PA_SU_POINT_SIZE`, …) | CONFIRMED |
| `+10680` | regs `0x2300`–`0x2325` (`PA_SC_LINE_CNTL`, `RB_COPY_*`, …) | CONFIRMED |
| `+10832` | regs `0x2380`–`0x2387` (`PA_SU_POLY_OFFSET_*`) | CONFIRMED |
| `+10896` | pointer read first by `EndTiling` | GUESS |
| `+10908` | **fence counter** (value returned by `InsertFence`) | CONFIRMED (`sub_827D4010` + trace) |
| `+10928` | last inserted fence | CONFIRMED (`sub_827D4010`) |
| `+10932/10936/10940` | render-target bookkeeping; `+10940` bit 5 / `+10943` bit 7 used by `Query::Issue` | LIKELY |
| `+12100` | per-sampler byte (filter state kept outside the fetch constant) | CONFIRMED (`sub_827D1310`) |
| `+12924` | tiling: tile count | CONFIRMED (`sub_827DCBF8` / `sub_827DCF68`) |
| `+12928` | tiling: tile rects, 16 bytes each | CONFIRMED |
| `+11992` | current vertex declaration | CONFIRMED (`sub_827E81E8`) |
| `+12612` | current index buffer object | CONFIRMED (read by draw, written by `sub_827D1B30`) |
| `+12616`..`+12628` | current **render-target surfaces** RT0-3 | CONFIRMED (`sub_827D2278`) |
| `+12632` | current depth-stencil surface | CONFIRMED (`sub_827D25C8` stores r4) |
| `+12904`..`+12920` | saved RT0-3 + depth (compared by `SetPredication` / flush) | LIKELY |
| `+15032` / `+15040` | default depth / default RT surface (state reset compares against them) | LIKELY (`sub_827D59D8`) |
| `+12868` | current **pixel shader** object | CONFIRMED (`sub_827E7E58`) |
| `+12872` | current **vertex shader** object | CONFIRMED (`sub_827E8018`) |
| `+12884` | predication / tile-select mask (emitted as `SET_BIN_MASK_LO`) | CONFIRMED (`sub_827DCA88`) |
| `+13436` | predication / sub-allocation for draw packets | LIKELY |
| `+13652` | saved ring pointer used by `EndVertices` | CONFIRMED |

Register names come from the SDK's `include/rex/graphics/register_table.inc`.
Because the shadow *is* Xenos register state, the native renderer can read
render state at draw time and does not need to hook every setter (some
setters are also inlined into game code).

## D3D functions

### Draw submission

| Address | Name | Notes | Conf. |
|---------|------|-------|-------|
Signatures below are `(r3 = device, r4, r5, …)`; "trace" = verified in the M1
runtime trace (see the section at the end).

| Address | Name / signature | Notes | Conf. |
|---------|------|-------|-------|
| `0x8291D328` | `D3DDevice_DrawIndexedVertices(prim, baseVertex, startIndex, indexCount)` | flushes all dirty groups, emits `DRAW_INDX` (`0xC0032201`); trace: prim 6 (strip), consecutive draws have start = prev start + prev count | CONFIRMED |
| `0x8291CF38` | `D3DDevice_DrawVertices(prim, startVertex, vertexCount)` | trace: 4-vertex strips (full-screen quads) | CONFIRMED |
| `0x8291CA58` | `D3DDevice_BeginVertices(prim, vertexCount, stride)` → **returns ring pointer** in r3 (not an HRESULT) | | CONFIRMED |
| `0x8291CA48` | `D3DDevice_EndVertices()` | restores ring ptr from `+13652` | CONFIRMED |
| `0x8291CEF0` | `D3DDevice_DrawVerticesUP(prim, vertexCount, pData, stride)` | calls `BeginVertices` (from `0x8291CF14`), **inlines** End; prim 13 = `QUADLIST` for UI | CONFIRMED |

Primitive types seen: 6 = `TRIANGLESTRIP` (all indexed world geometry, so
the index-buffer restart index matters), 13 = `QUADLIST` (UI), 1 =
`POINTLIST`. A native backend must expand quad lists itself.

### Clear / resolve / tiling / present

| Address | Name / signature | Conf. |
|---------|------|-------|
| `0x827EDC18` | `D3DDevice_Clear(count, pRects, flags, color, Z=f1, stencil)`; flags seen `0x1`, `0xF`, `0x20`, `0x30`, `0x31` = 360 encoding (`TARGET0..3`=bits 0-3, `ZBUFFER`=`0x10`, `STENCIL`=`0x20`); pRects is always the global full-screen rect `0x82C25868` | CONFIRMED |
| `0x827E5B18` | **`D3DDevice_Resolve(flags, pSourceRect, pDestTexture, pDestPoint, destLevel, destSlice, pClearColor, ClearZ=f1, …)`**; flags 0 = RT0, 4 = depth-stencil; pSourceRect = `0x82C25868`; dest textures are static objects at `0x82BCxxxx` | CONFIRMED (was "resolve core") |
| `0x827DCBF8` | **`D3DDevice_BeginTiling(flags, count, pTileRects, pClearColor, ClearZ=f1, …)`**: stores count at `+12924`, rects at `+12928`; trace: 1 call/frame in gameplay, count = 1 | LIKELY (was "SetRenderTarget") |
| `0x827DCF68` | **`D3DDevice_EndTiling(resolveFlags, pResolveRects, pDestTexture, pClearColor, ClearZ=f1, …)`**: loops over the tile rects; trace: flags `0x14`, same pClearColor as BeginTiling | LIKELY (was "Resolve") |
| `0x827D2278` | **`D3DDevice_SetRenderTarget(index, pSurface)`**: stores at `+12616+4*index`, copies `surface+0x1C` into `RB_COLOR_INFO` (`RB_COLOR1..3_INFO` for index > 0). 8 call sites; trace: RT0 is set to static surfaces `0x82BC03E8` (1280×720), `0x82BC043C` (640), `0x82BC0490` (640) | CONFIRMED |
| `0x827DCA88` | **`D3DDevice_SetPredication(mask)`**: `+12884 = mask`, emits `SET_BIN_MASK_LO` (`0xC0006000`); r4 = 0 disables, 3 during `EndTiling` | CONFIRMED |
| `0x827D59D8` | device state reset: `SetRenderTarget(i, NULL)` for RT1-3 unless default, `SetDepthStencilSurface(NULL)`, NULL shaders / decl / indices, clears samplers | CONFIRMED |
| `0x827D25C8` | `D3DDevice_SetDepthStencilSurface(pSurface)` (stores `+12632`; writes `RB_DEPTH_INFO`, `RB_SURFACE_INFO`, `RB_HIZCONTROL`); trace: NULL or static surfaces `0x82BC0394` (1280×720), `0x82BC0490`, `0x82BC043C` | CONFIRMED |
| `0x827D4AB0` | called once per frame just before `Swap` with r4=0; sets up the 1280×720 back buffer (via `0x827DCA88`) | GUESS |
| `0x827D4EE0` | `D3DDevice_Swap(pFrontBuffer, pParameters)` (calls `VdSwap`); trace: front buffer alternates `0x82BC0E90` / `0x82BC0EF4` (double-buffered), pParameters NULL | CONFIRMED |
| `0x827D4ED8` | tail-calls `0x827D4AB0` with r4=0 | CONFIRMED (code) |
| `0x827E2FC0` | `Direct3D_CreateDevice` (`VdInitializeEngines`, interrupt callback, ring init `0x827D4290`) | CONFIRMED |

The static surface objects at `0x82BC0388`..`0x82BC04E4` are **re-initialised
between uses** (e.g. `0x82BC043C` is a 640-pitch colour target and later a
960-pitch depth surface in the same frame), so a native renderer must re-read
the surface header at every bind instead of caching per object.

### Sync and queries

| Address | Name / signature | Conf. |
|---------|------|-------|
| `0x827D4010` | `D3DDevice_InsertFence()`: returns `dev+10908`, stores it at `+10928`, kicks the ring (`0x827D3F10`). Trace: fence values step by 2 | CONFIRMED |
| `0x827D3178` | `BlockOnFence(fence)` (no device arg: fetches the global device and calls `0x827D3060(dev, fence, 6, 0, 0)`); trace: waits on the first fence inserted in the same frame | CONFIRMED |
| `0x827DAA98` | `D3DQuery_Issue(pQuery, flags)`: r3 = query object (`[0]` = device, `+4` = type, 9 = `D3DQUERYTYPE_OCCLUSION` per the dump), flags 2 = BEGIN, 1 = END; emits `EVENT_WRITE` (`0xC0004600`). Trace: 1 occlusion query per frame in the menu, 2 in gameplay | CONFIRMED |

### Dynamic buffers (worker threads)

| Address | Name / signature | Conf. |
|---------|------|-------|
| `0x827D71A8` | vertex-buffer **lock**: reads the fetch header at `+24` (base) / `+28` (size), calls `0x827D6DE0`; returns the CPU pointer. Trace: 9/frame from `0x8250F708` on 3 worker threads, sizes `0x2700`–`0x20180` | LIKELY (was "AddRef?") |
| `0x827D6268` | matching **unlock** (base from `+24` → `0x827D5E70`); called from `0x8250F7EC` right after each lock | LIKELY |

### Shaders, vertex input, textures

| Address | Name | Conf. |
|---------|------|-------|
| `0x827E8018` | `D3DDevice_SetVertexShader(pShader)` (`+12872`) | CONFIRMED |
| `0x827E7E58` | `D3DDevice_SetPixelShader(pShader)` (`+12868`) | CONFIRMED |
| `0x827E82F0` | `D3DDevice_CreateVertexDeclaration` (12-byte elements, 0xFF terminator) | CONFIRMED |
| `0x827E81E8` | `D3DDevice_SetVertexDeclaration(pDecl)` (`+11992`) | CONFIRMED |
| `0x827D1B30` | `D3DDevice_SetIndices(pIndexBuffer)` (`+12612`) | CONFIRMED |
| `0x827D1A10` | `D3DDevice_SetStreamSource(stream, pVB, offset, stride, dirtyBit)`: stream s writes **vertex fetch constant 95-s** at `+1152 + (95-s)*8` (dword0 = GpuAddr(vb[+0x18] + offset), dword1 = vb[+0x1C] - offset); current VB at `+12636 + 4*s` | CONFIRMED |
| `0x827D92A0` | `D3DDevice_SetTexture(sampler, pTexture, dirtyBit)`: r6 = `0x80000000 >> sampler` (fetch dirty bit computed by the caller); trace only shows the null-reset loop | LIKELY |
| `0x827D0EC8`, `0x827D1070` | sampler filter setters `(sampler, value)`: patch fetch dwords 3/4 through a table at `0x82024C28` indexed by `+12100[sampler]` | LIKELY |
| `0x827D1310` | sampler setter `(sampler, value)`: stores `+12100[sampler] = value`, patches fetch dword 3 bits 4-6 (mip filter / anisotropy) | LIKELY |
| `0x827D1430` | `SetSamplerState(MIPMAPLODBIAS)`: `(sampler, float bits)` scaled to fixed point into fetch dword 4 bits 12-21 (`lod_bias`) | CONFIRMED (code) |
| `0x827E7B10` | `SetVertexShaderConstantF(start, pData, count)`: copy to `+1920 + 16*start`, OR dirty `+0` | CONFIRMED |
| `0x827E7BE8` | `SetPixelShaderConstantF(start, pData, count)`: copy to `+6016 + 16*start`, OR dirty `+8` | CONFIRMED |
| `0x8291D778` | **`LoadShaderConstantsFromBuffer(type, start, count, pBuffer, offset)`** (TU2): emits `LOAD_ALU_CONSTANT` from `pBuffer[+0x18] + offset*16` into vec4 `type*256 + start`, `count*4` dwords; bypasses the shadow. Only caller: engine `0x824D9308`, which first clears the matching dirty bits | CONFIRMED (code + M2) |
| `0x827D7128` | resource `Release` (atomic refcount); never hit during the traced frames | LIKELY |

### Render-state setters (CONFIRMED by register written)

| Address | Register | Call sites |
|---------|----------|-----------|
| `0x827CF628` | `PA_SU_SC_MODE_CNTL` (cull/fill) | 68 |
| `0x827CF6C0`, `0x827CF750`–`0x827CFA50` | `RB_BLENDCONTROL0..3` | 72 / 1–2 |
| `0x827CFB78` | `RB_BLEND_RED/GREEN/BLUE/ALPHA` | 7 |
| `0x827CFAE8` | `RB_ALPHA_REF` | 1 |
| `0x827CF688`, `0x827CFB48` | `RB_COLORCONTROL` | 1 |
| `0x827CFD50`, `0x827CFD90`, `0x827CFDC0`–`0x827CFF10` | `RB_DEPTHCONTROL` (z enable/write/func, stencil) | 72 / 67 / 10–15 |
| `0x827D0010`/`30`/`50` | `RB_STENCILREFMASK` bytes | 12 / 12 / 4 |
| `0x827D0130`, `0x827D01F8` | `PA_SU_POLY_OFFSET_*` | 8 |
| `0x827D02F0` | `RB_COLOR_MASK` | 20 |
| `0x827D08A8` | `RB_COLOR_INFO` (sRGB / format bits) | 11 |
| `0x827D0C18` | `VGT_MULTI_PRIM_IB_RESET_INDX` | 1 |
| `0x827D0DE8`–`0x827D0E78` | `RB_HIZCONTROL` | 1 |

### Internal plumbing (do not call from native code)

| Address | Role |
|---------|------|
| `0x827E92E0` | flush one dirty register group as type-0 packets |
| `0x827E9520` | flush dirty fetch constants (6 dwords each, reg `0x4800`) |
| `0x827E9678` | flush dirty ALU constants |
| `0x827EA4A0` | shader / program state flush (`SQ_PROGRAM_CNTL`, `SQ_CONTEXT_MISC`, `RB_MODECONTROL`) |
| `0x827E8F48` | shader-dependent state flush |
| `0x827D3F10` | ring segment kick (emits `INDIRECT_BUFFER` via `0x827D3568`) |
| `0x827D4290` | ring buffer init (`ME_INIT`, `VdInitializeRingBuffer`) |

## Resource object layouts (from code + `D3D-OBJ` dumps, log 318)

All resources start with the XDK `D3DResource` header:

| Offset | Field |
|---|---|
| `+0x00` | Common: **low byte = type** (1 VB, 2 IB, 3 texture, 4 surface, 5 vertex decl, 6 VS, 7 PS); upper bits flags |
| `+0x04` | reference count |
| `+0x08` | fence (last GPU use; `Set*Shader` writes the current fence into the old object) |
| `+0x0C` | read fence |
| `+0x10` | identifier |
| `+0x14` | base flush (`FFFF0000`) |

| Type | Layout after the header | Conf. |
|---|---|---|
| Vertex buffer (1) | `+0x18`..`+0x1F` = Xenos **vertex fetch constant** (dword0 = base \| 3, dword1 = size in dwords << 2 \| endian); `+0x20` = base address (CPU virtual, `0xA…` physical heap), `+0x24` = size in bytes. `VB_Lock` returns `+0x20` | CONFIRMED |
| Index buffer (2) | `+0x18` = base address, `+0x1C` = size in bytes (`+0x20/+0x24` repeat them); Common bit 29 set in all seen | LIKELY |
| Texture (3) | `+0x18` = mip flush, **`+0x1C`..`+0x33` = 6-dword Xenos texture fetch constant** (dword1 = base \| format, dword2 = size: (w-1) \| (h-1)<<13, dword5 = mip base). Ex.: `0x82BDD220` = 1024×1024 DXT1 | CONFIRMED |
| Surface (4) | `+0x18` = `RB_SURFACE_INFO` value (pitch \| hiz pitch << 18), `+0x1C` = `RB_COLOR_INFO` / `RB_DEPTH_INFO` value (EDRAM base tile \| format << 16), `+0x2C` = size in bytes, `+0x30` = EDRAM base tile, `+0x34` = size in EDRAM tiles, `+0x38` = HiZ base (`FFFFFFFF` for colour), `+0x44` = parent | CONFIRMED (`+0x1C` from `SetRenderTarget` code; rest from dumps) |
| Vertex decl (5) | element data from `+0x18`; created by `CreateVertexDeclaration` | LIKELY |
| Pixel shader (7) | `+0x18` = **microcode base** (e.g. `0xFF5B8000`); `blk = PS + [PS+0x40]`; ucode address = `base + blk[+40]`, size bytes = `blk[+44]`, loaded with PM4 `IM_LOAD` (`0xC0002700`) in the program flush `0x827EA4A0`; `SetPixelShader` applies the embedded constant patch block at `PS + 40 + [PS+60]` | CONFIRMED (code) |
| Vertex shader (6) | `+0x20` = **microcode base**; the program flush picks a bound **variant** v (per vertex declaration): `blk = VS + VS[+0x380 + 8*v]`, ucode address = `base + blk[+872]`, size bytes = `blk[+876]` (IM_LOAD type 0). Per-variant 416-byte records at `VS + 416*v` (fence at `+64`). `SetVertexShader` applies the constant patch block at `VS + 872 + [VS+892]`; flags at `+872`, alternate block at `+904` | CONFIRMED (code) |

Guest -> GPU address conversion used everywhere (stream sources, IM_LOAD):
`gpu = (a & 0x1FFFFFFF) + (((a >> 20) + 0x200) & 0x1000)`, i.e. physical
address plus the 4 KB offset for the `0xE0000000` view.

Front buffers (`Swap` r4) `0x82BC0E90` / `0x82BC0EF4`: 1280×720 `8_8_8_8`
textures at `0xAA001000` / `0xAA39A000`. EDRAM plan for the main 1280×720
pass: depth at tile 0 (720 tiles), colour at tile 720.

## Effective GPU state at a draw (verified by M2)

The register file the GPU sees at a draw is **not** just the device shadow.
M2 (`src/native_renderer/native_observer.cpp`) reproduces it exactly, for every
register the bound shaders read, with these rules (log 323: menu 66/66,
gameplay 948/948, pause 690/690 draws identical to the Xenos register file):

1. **Render state** (`0x2000`–`0x2387` groups): the shadow ranges listed in
   the device layout. Not shadowed: `RB_SAMPLE_COUNT_ADDR` (`0x2325`, written
   by `Query::Issue`).
2. **Fetch constants** (`0x4800`, shadow `+1152`): the shadow, except for
   inline draws (below).
3. **Float constants**: per 4-vec4 group there are two sources.
   - Shadow: at every draw the D3D flush uploads each group whose dirty bit is
     set (`dev+0` VS / `dev+8` PS, **bit 63-g for group g = vec4 >> 2**).
   - Engine constant buffers: `0x824D9308` clears the dirty bits of a range
     and calls `0x8291D778`, so the GPU reads `pBuffer[+0x18] + offset*16`.
   A group's value is whichever happened last.
4. **Shader literal constants**, applied last by the program flush
   (`0x827EA218`, called for VS with `blk = VS+872`, base `VS[+0x20]`; for PS
   with `blk = PS+40`, base `PS[+0x18]`): records `{u16 vec4, u16 dwords,
   u32 data offset}` at `blk + [blk+20] + 20`, terminated by `dwords == 0`,
   loaded with `LOAD_ALU_CONSTANT` from `base + data offset`. Literals do not
   update the shadow, so they **persist in the GPU registers** until a
   dirty-group flush or a buffer load overwrites that vec4; later shaders read
   them (e.g. VS c245-c247 set by an earlier shader, read by the character
   skinning VS). `gpu_state` keeps this stale-literal state.
5. **Inline draws**: `BeginVertices`..`EndVertices` is one draw and its packet
   is written at `EndVertices` (`DrawVerticesUP` calls `BeginVertices` and
   inlines the end). Stream 0 = vertex fetch constant 95 (`0x48BE`) points at
   the ring data `BeginVertices` returned.

Draws issued internally by D3D (clears, resolves; prim 8 = RECTLIST, 3
vertices, `RB_MODECONTROL` 4/5/6) have no game draw call and are expected to
leave stale state the game's shaders do not read.

### Vertex shader microcode actually loaded

D3D patches the vertex fetch instructions of a VS **per vertex declaration**
(format, offset, stride from `dev+12704`). The program flush (`0x827EA4A0`)
binds a variant with `0x827EA360`: if variant v's record (`VS + 416*v`) has
the declaration id at `+40` and matching stream masks, it is reused;
otherwise it is re-patched in place by `0x827E97C8`, unless the GPU still
uses it (fence at `+64`), in which case `0x827EA0C8` copies the ucode into the
ring as `IM_LOAD_IMMEDIATE` and patches the copy. The GPU runs the **last
vertex `IM_LOAD` / `IM_LOAD_IMMEDIATE` the flush writes**; renderer=dante
scans the flush's packets for it (segment switches via `0x827D3F10` /
`0x827D4148` restart the scan). Reading the in-memory variant instead yields
the wrong vertex layout for busy variants (stretched geometry).

### Resolve details

- `RB_COPY_DEST_INFO.copy_dest_swap` (bit 24) swaps red/blue on the copy; the
  game sets it for its 8888 resolves (front buffers are BGR, fetch swizzle
  ZYX1). Resolve writes the copy registers, then resets them to 0, so the
  value in effect is the last non-zero write in the Resolve's packets.
- Polygon offset: `PA_SU_SC_MODE_CNTL.poly_offset_*` with
  `PA_SU_POLY_OFFSET_*` is applied as host depth bias exactly like the SDK
  Vulkan host-RT path (`draw_util::GetPreferredFacePolygonOffset`).

## Frame shape (from `gpu_pass_trace_frames`, log 313, in-game)

~19 render-target segments per frame, ~400 draws, ~600k indices. The main
1280-pitch colour pass has 182 draws / 35 distinct shaders; several 640/320
pitch half-res passes; 1-draw 3-index segments are resolve/clear quads.
Main passes run without MSAA, so no predicated tiling has been observed.

## M1 runtime trace results (2026-10-07, `dantes_inferno_316.log`)

Three F8 captures of 3 frames each: main menu, gameplay, pause menu. Per-frame
call counts were **identical across the 3 frames of each capture**, so the
frame structure is deterministic for a static scene.

| Calls / frame | Menu | Gameplay | Pause |
|---|---:|---:|---:|
| `DrawIndexedVertices` | 0 | 239 | 0 |
| `DrawVertices` | 2 | 21 | 185–186 |
| `DrawVerticesUP` | 18 | 2 | 43 |
| `BeginVertices` / `EndVertices` | 20 / 2 | 56 / 54 | 44 / 1 |
| `Clear` / `Resolve` (`0x827E5B18`) | 6 / 6 | 7 / 10 | 3 / 3 |
| `BeginTiling` / `EndTiling` | 0 | 1 / 1 | 0 |
| `InsertFence` / `BlockOnFence` | 6 / 4 | 8 / 1 | 3 / 4 |
| `Query::Issue` | 2 | 4 | 0 |
| `SetVertexShader` / `SetPixelShader` | 7 / 32 | 56 / 78 | 20 / 92 |
| `SetTexture` / `SetStreamSource` | 58 / 57 | 320 / 334 | 154 / 264 |
| `SetIndices` | 2 | 241 | 2 |
| VB lock / unlock (worker threads) | 0 | 9 / 9 | 0 |
| `Swap` | 1 | 1 | 1 |

Findings:
- One render thread; the device pointer is constant within a run
  (`0xD8565E00` this run; heap address, so it is not fixed across runs).
- `BeginVertices = DrawVerticesUP + EndVertices` in every capture, so
  `DrawVerticesUP` calls `BeginVertices` and inlines `EndVertices`.
- The engine starts each frame by resetting textures 0..N and streams 0..5 to
  NULL (`0x824D6E8C`, `0x824D6ED8`), and shaders to NULL (`0x824D6E54/60`).
- `CreateDevice` and `Release` are not called in steady state.
- Worker threads lock and fill dynamic vertex buffers each gameplay frame. A
  native renderer must treat a VB unlock as "upload this range".

## Open questions

1. `0x827D4AB0` role (pre-Swap; restores the 1280×720 back buffer).
2. Whether any game code writes PM4 directly or replays recorded command
   buffers (static evidence says no; the M2 observer's unmatched draws are the
   previous frame still executing when a capture starts, plus D3D-internal
   clears / resolves).

Closed: VS variant choice (see "Vertex shader microcode actually loaded").
Closed by the traces: argument order of `Clear`, `Resolve`, `BeginVertices`,
the draw calls, `Swap`, `SetRenderTarget`, `SetTexture(sampler, pTex, dirtyBit)`;
the fence and query APIs; texture / surface / buffer / PS object layouts.
