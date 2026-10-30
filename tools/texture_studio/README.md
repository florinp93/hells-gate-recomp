# Texture Studio

Extracts the game's textures to PNG for upscaling or editing and turns the
edited files into a texture mod the game loads instead of the originals.
Mods need the native renderer (`--renderer=dante`, the default). The
step-by-step guide for modders is `docs/texture_modding.md`.

## Workflow

1. Run `TextureStudio.exe` (or `python -m texture_studio` from `tools/`).
2. Pick the game folder (the one with `bigfile0.viv`) and an empty project
   folder, then **Scan game**. The scan indexes every 2D texture (about 4300
   unique ones) in under a minute.
3. Filter by name or group (`hero_*_c`, `limbo`), format or size, and
   **Export** the textures you want. PNGs land in the project folder,
   grouped by the game's texture groups.
4. Upscale or edit the PNGs in place. Any resolution works; keep the aspect
   ratio and the file names.
5. On **Build mod**, set a name and the `mods` folder next to
   `dantes_inferno.exe`, then **Build mod**. Only edited PNGs are written.

The game loads every folder under `mods/` at startup, in alphabetical order;
when two mods replace the same texture the later one wins. Delete a mod's
folder to remove it.

## Mod layout

```
mods/<ModName>/mod.toml
mods/<ModName>/textures/<name>_<hash>.dds
```

`<hash>` is the 16-digit texture hash the game computes when it uploads the
original texture. Files can be renamed as long as they end in the hash.
Supported DDS formats: RGBA8/BGRA8, BC1, BC2, BC3, BC4, BC5, BC7, with or
without mips. The studio writes RGBA8 with a full mip chain; BC7 files from
other tools (texconv, NVIDIA Texture Tools) load as well and use a quarter of
the memory.

## Notes

- Normal maps (`DXT5_NM`, stored as X in alpha and Y in green) are exported
  as standard RGB normal maps and converted back, renormalised, on build.
- Some textures appear under several names because the game stores identical
  copies; they share one hash and one PNG.
- Cube maps and volume textures are not listed.
- `mod_capture=true` (cvar) logs the hash of every texture the game uploads,
  to find textures that are on screen.
- `mods_enabled=false` disables all mods.

## Command line

```
python -m texture_studio scan <game> <project>
python -m texture_studio export <game> <project> --name "hero_*"
python -m texture_studio build <project> <mods>/<ModName>
```

Requirements for running from source: `pip install -r requirements.txt`.
`build.bat` packages `out/texture_studio/dist/TextureStudio.exe`.

## Texture format

TG4H headers are big-endian: TG4D size at 0x1C, width and height (u16) at
0x20, mip count at 0x26, then a string table ending in the format (`DXT1c`,
`DXT1a`, `DXT5`, `DXT5_NM`, `RGBA8`, `L8`, ...), flags (`sRGB`, `aniso`,
`cube`, `3d`, `clamp`), texture group and name. TG4D data is linear (not
tiled), byte-swapped in 16-bit words for DXT, with all mips packed tightly.
The mod hash is XXH3-64 of byte-swapped level 0, seeded with
`width | height << 16 | xenos_format << 32`.
