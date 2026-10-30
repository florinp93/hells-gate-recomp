# Texture Modding Guide

Texture Studio extracts Dante's Inferno's textures as PNG files, lets you
upscale or repaint them with any tool, and packs the results into a texture
mod. The game loads mods at startup and draws your textures in place of the
originals. Nothing in the game files is changed.

## Requirements

- Hell's Gate with texture mod support (the first release after
  v0.8.5-beta), using the default native renderer.
- An installed copy of the game: the folder with `dantes_inferno.exe` and
  `game\bigfile0.viv` (by default `C:\Games\DantesInferno`).
- Windows 10 or 11. `TextureStudio.exe` needs no install.

## 1. Set up a project

1. Start `TextureStudio.exe`.
2. **Game folder**: the `game` folder inside the install, e.g.
   `C:\Games\DantesInferno\game`.
3. **Project folder**: an empty folder for your work, e.g.
   `D:\Mods\DanteHD`. Keep one project per mod.
4. Click **Scan game**. It indexes every texture (about 4,300) in under a
   minute. The index is saved in the project, so this is needed once.

## 2. Find textures

The **Textures** tab lists everything the scan found. Click a row to
preview it.

- **Filter**: type part of a name, texture group or hash. Wildcards work:
  `hero_*_c` gives Dante's colour maps, `limbo` every Limbo texture.
- **Format**: colour (DXT1), colour with alpha (DXT5), normal maps,
  uncompressed.
- **Size**: hide small textures.
- **Exported only**: what is already in your project.

Name suffixes tell you what a texture is for:

| Suffix | Meaning |
|---|---|
| `_c` | colour (diffuse) |
| `_ca` | colour with alpha (cut-outs, foliage, hair) |
| `_n` | normal map |
| `_s` | specular |

Some textures are listed under several names because the game stores
identical copies. They are one texture, and replacing it changes all of
them.

## 3. Export

Select rows and click **Export selected**, or **Export all shown** to
export everything matching the filters. The PNGs go into the project
folder, one subfolder per texture group, e.g.
`chars_hero_texture\hero_body_c_9BC16148.png`. **Open PNG** and **Open
project folder** take you there.

## 4. Edit or upscale

Edit the PNGs where they are: same folder, same file name.

- **Any resolution works**: 2x, 4x, or a different size, as long as the
  aspect ratio stays the same. Keep power-of-two sizes (512, 1024, 2048,
  4096).
- Upscalers such as chaiNNer, Upscayl or ESRGAN models can process whole
  folders. Write the results back over the exported files.
- **Alpha**: `_ca` and DXT5 textures use alpha for transparency or
  masks. Keep it; most upscalers can process alpha separately.
- **Normal maps** are exported as standard tangent-space normal maps (the
  usual blue/purple look), so normal-map upscaling models and tools work on
  them. Texture Studio converts them back to the game's own layout when it
  builds the mod and renormalises them.

Textures you didn't touch are left out of the mod automatically: only files
that differ from the export are packed.

## 5. Build the mod

On the **Build mod** tab:

1. Enter **Mod name**, **Author** and **Version**.
2. **Mods folder**: the `mods` folder next to `dantes_inferno.exe`, e.g.
   `C:\Games\DantesInferno\mods`. If you leave it empty it is set from the
   game folder. The folder is created if it doesn't exist.
3. **Find edited textures** lists what will be packed.
4. **Build mod** writes `mods\<Mod name>\` with a `mod.toml` and one DDS file
   per texture. Warnings (e.g. a changed aspect ratio) appear in the log.

Rebuild after further edits; files are overwritten in place.

## 6. Play

Start the game as usual. The game log (`logs` folder) lists every mod it
loaded and each texture as it is replaced:

```
ModLoader: DanteHD (42 textures)
ModLoader: 9BC16148BDFAF412 -> hero_body_c_9BC16148BDFAF412.dds (2048x2048, 12 levels)
```

## Installing and sharing mods

A mod is just its folder. To share one, zip `mods\<Mod name>` and the
player unzips it into their own `mods` folder. To remove a mod, delete its
folder.

All folders in `mods` are loaded in alphabetical order. When two mods
replace the same texture, the one that sorts last wins; prefix folder names
with numbers (`10_UI`, `20_Characters`) to control the order.

## Advanced

### Mod folder format

```
mods\<Mod name>\mod.toml
mods\<Mod name>\textures\<anything>_<hash>.dds
```

The game matches files by the 16-digit hash at the end of the file name, so
files can be renamed as long as the hash stays. Supported DDS formats:
RGBA8, BGRA8, BC1, BC2, BC3, BC4, BC5 and BC7, with or without mipmaps.
Texture Studio writes uncompressed RGBA8 with full mipmaps (best quality,
largest files). For big packs, recompress with texconv to BC7
(`texconv -f BC7_UNORM -m 0 file.dds`), which uses a quarter of the memory.
Normal maps must stay in the game's layout (X in alpha, Y in green), so
recompress the DDS files Texture Studio wrote rather than your PNGs.

### Finding a texture you can see in game

Launch the game with `--mod_capture=true` (launcher launch options). The log
then lists the hash, size and format of every texture as the game loads it.
Paste a hash into Texture Studio's filter box to find that texture. The
textures logged right after entering an area are the ones it uses.

### Turning mods off

`--mods_enabled=false` starts the game without loading any mod.

### Command line

The same steps without the GUI (from the source in `tools/`):

```
python -m texture_studio scan <game folder> <project>
python -m texture_studio export <game folder> <project> --name "hero_*"
python -m texture_studio build <project> <mods folder>\<Mod name>
```

## Limitations

- Cube maps (reflections) and volume textures (colour grading) are not
  listed yet.
- Mods need the native renderer; the Xenos fallback renderer ignores them.
- Videos are not covered.
