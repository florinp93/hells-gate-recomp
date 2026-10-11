# Frame rate above 60 fps

Run with `--video_mode_refresh_rate=120` (or 180, 240). Use multiples of 60:
the UI updates at 60 Hz and judders at other rates.

## How the game paces and times frames

- Display init `sub_824D6A90` calls `XGetVideoMode` and stores the refresh
  rate as the game rate (`0x82C98758`) and timebase ticks per frame
  (`0x82C98750`). `sub_821CF858` derives the simulation constants from it
  (1/rate at `0x82CBEB2C` / `0x82CBEC38`, rate copies, integer rate).
- The guest vblank callback `0x824D6DF8` copies the vblank count to
  `0x82CA892C`. The present function `sub_824DB9E0` can wait for N vblanks per
  frame (stack of N at `0x82CA8948`, depth at `0x829AFE24`), but N is 0 in
  play: the frame rate follows the vblank rate (`video_mode_refresh_rate`).
- Gameplay, cutscenes and FMV follow the game rate and real time, so they are
  correct at any rate.

## UI

- The per-frame UI tick `sub_821D07F0` polls input (`sub_8252EDC0`), then the
  UI step `sub_8256E9D8` sends the per-frame message `0x82CC8930` (ids at
  `0x82CC8974` / `0x82CC8954`). Its listeners include gameplay objects and the
  APT manager handler `sub_82629450`.
- That handler runs all UI: screen input and menu logic (`0x82633090`),
  screen updates such as the absolve minigame (`0x82633540` ->
  `sub_823D0AF0`), and the APT (Flash) advance `sub_826516B8`, which is passed
  a fixed 20 time units per update.
- The UI counts updates, so it runs fast above 60 fps. Gating only parts of it
  (the APT advance, or scaling its time) desynchronises menu logic and movies
  (invisible menus); gating the whole message slows gameplay.
- Fix (`src/frame_rate_hooks.cpp`): the handler runs for that message at 60 Hz,
  with the frame time (`0x82CBEB60`) set to the real time since its last run
  and each pad's previous buttons (input manager `+60 + pad * 68`, buttons at
  `+0`, previous at `+2`) set to the buttons at its last run, so presses on
  skipped frames are not lost.
