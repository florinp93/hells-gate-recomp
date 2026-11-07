# Button glyph icons

Kenney's "Input Prompts" pack, version 1.1, licensed CC0 1.0 (see `LICENSE.txt`).

- Source: https://kenney.nl/assets/input-prompts (128 px "Double" PNGs, taken from
  the CC0 GitHub mirror https://github.com/tanuki-billie/kenney-input-prompts)
- Used: PlayStation face buttons, triggers, sticks, d-pad, Share / Create /
  Options, and the keyboard and mouse keys (no outline variants).

Every PNG in this folder is embedded in the executable at build time
(`CMakeLists.txt`, `glyph_icons.cpp`) and looked up by file name in
`src/renderer/button_glyphs.cpp`. Adding a key means dropping its PNG here and
mapping the SDK key name to it in `kKeyIcons`.
