#!/usr/bin/env python3
"""Generate docs/command_line_flags.md from the REXCVAR_DEFINE_* macros.

The page lists every cvar the executable registers: the ones defined in
src/ and the ones inherited from the ReXGlue SDK. The SDK tree must be the
one setup.ps1 / setup.sh leave in thirdparty/rexglue-sdk, with the patches
from patches/sdk/ applied: they add and change cvars (fullscreen_exclusive,
gpu_host_rendering, the gpu_3d_to_2d_texture default, ...), and the game is
built against that tree.

    python tools/gen_flags_doc.py            # rewrite docs/command_line_flags.md
    python tools/gen_flags_doc.py --check    # exit 1 if the page is out of date

The Values column comes from each cvar's .allowed() / .range(). Flags that
declare neither get their values from the tables below (VALUES, PREFIX_VALUES),
written from the code that reads them; RANGE_NOTES adds units to a range. The
prose at the top of the page is the INTRO template at the bottom of this file.
"""

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SOURCE_EXTENSIONS = {".cpp", ".cc", ".h", ".hpp"}
SKIP_DIRS = {"tests", "test", "thirdparty", "third_party", "out", "build"}
# The codegen library and the rexglue CLI are build-time tools; their cvars
# are never registered in the game.
SDK_SKIP_DIRS = {"codegen", "rexglue"}

TYPE_NAMES = {
    "BOOL": "bool",
    "INT32": "int",
    "INT64": "int64",
    "UINT32": "uint",
    "UINT64": "uint64",
    "DOUBLE": "double",
    "STRING": "string",
}
TYPE_FALLBACK_VALUES = {
    "bool": "`true`, `false`",
    "int": "integer",
    "int64": "integer",
    "uint": "integer >= 0",
    "uint64": "integer >= 0",
    "double": "decimal",
    "string": "text",
}
# Preprocessor symbols that show up in default values. The game does not
# build for macOS.
PLATFORM_DEFINES = {"REX_PLATFORM_MAC": False}

# Accepted values of flags that declare no .allowed() / .range(), from the
# code that reads them.
VALUES = {
    # Hell's Gate
    "achievement_sound_file": "`.wav` file path (empty = built-in chime)",
    "achievement_sound_volume": "`0`-`100`",
    "dante_resolution": "`WIDTHxHEIGHT` (e.g. `1920x1080`, `3440x1440`), `auto`, "
                        "empty (use dante_resolution_scale)",
    "dlc_source_path": "folder path (empty = no DLC)",
    "glyph_family": "`auto`, `xbox360`, `playstation4`, `playstation5`, `keyboard`",
    "native_render_scale": "`off`, `auto`, `1`-`7`",
    "renderer": "`dante`, `xenos`, `native`",
    "time_scalar": "decimal > 0 (`1.0` normal, `50.0` fast-forward)",
    "ultrawide_target_aspect": "`0` (off) or the target ratio as a decimal: `1.3333` (4:3), "
                               "`1.7778` (16:9), `2.3704` or `2.3889` (21:9), `3.5556` (32:9)",
    # ReXGlue SDK
    "audio_maxqframes": "`4`-`64`",
    "cache_root": "folder path",
    "cl": "text (extra command line passed to the guest)",
    "d3d12_adapter": "`-1` (any), `-2` (WARP) or adapter index >= 0",
    "dump_path": "folder path",
    "dump_shaders": "folder path",
    "dump_source": "file path",
    "fullscreen_exclusive": "`WIDTHxHEIGHT` display mode (e.g. `2560x1440`); empty = borderless "
                            "fullscreen at the desktop resolution",
    "game_data_root": "folder path (required)",
    "gpu_plugin": "`xenos` or empty",
    "gpu_vp6_c1_mode": "`0` (pixel), `1` (mode A), `2` (mode B)",
    "hid_mappings_file": "file path",
    "license_mask": "integer (bit mask)",
    "log_file": "file path (empty = automatic)",
    "metadata_root": "folder path",
    "perf_log_csv": "file path (empty = off)",
    "render_target_path_d3d12": "`rtv`, `rov`, empty (auto)",
    "render_target_path_vulkan": "`fsi`, empty (auto)",
    "resolution": "`720p`, `1080p`, `1440p`, `4k` or `WIDTHxHEIGHT`",
    "update_data_root": "folder path",
    "user_country": "XGetUserCountry numeric ID (e.g. `103` = US)",
    "user_data_root": "folder path",
    "user_language": "XLANGUAGE ID: `1` EN, `2` JA, `3` DE, `4` FR, `5` ES, `6` IT, `7` KO, "
                     "`8` ZH, `9` PT",
    "video_driver": "`wayland`, `x11`, `windows` or empty (SDL default)",
    "vulkan_device": "`-1` (auto) or device index >= 0",
}
PREFIX_VALUES = {
    "keybind_": "key or mouse button name(s), comma-separated; modifiers with `+` "
                "(e.g. `Q,I`, `Shift+Up`, `LMB`, `Mouse4`); empty = unbound",
}
# Units appended to a declared range.
RANGE_NOTES = {
    "anisotropic_override": "(`-1` no override, `0` off, `1`-`5` force 1x, 2x, 4x, 8x, 16x)",
    "log_flush_interval": "(seconds)",
    "log_max_file_size_mb": "(MB)",
    "present_fsr_sharpness_reduction": "(stops)",
    "rexcrt_heap_size_mb": "(MB)",
    "texture_cache_memory_limit_hard": "(MB)",
    "texture_cache_memory_limit_render_to_texture": "(MB)",
    "texture_cache_memory_limit_soft": "(MB)",
    "texture_cache_memory_limit_soft_lifetime": "(seconds)",
    "video_mode_height": "(pixels)",
    "video_mode_refresh_rate": "(Hz; use multiples of 60: 60/120/180/240)",
    "video_mode_width": "(pixels)",
    "window_height": "(pixels)",
    "window_width": "(pixels)",
}

MACRO_RE = re.compile(r"\bREXCVAR_DEFINE_(BOOL|INT32|INT64|UINT32|UINT64|DOUBLE|STRING)\s*\(")
STRING_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')
NUMBER_RE = re.compile(
    r"^[-+]?(?:0[xX][0-9a-fA-F]+|\d+\.\d*(?:[eE][-+]?\d+)?|\.\d+|\d+(?:[eE][-+]?\d+)?)[uUlLfF]*$")
CAST_RE = re.compile(r"^(?:static_cast<[^>]+>|[A-Za-z_]\w*)\s*\((.*)\)$", re.S)
CONSTANT_RE = re.compile(r"\b(k[A-Z]\w*)\s*=\s*([^;,}\n]+)")
UNRESOLVED = "(see code)"


class Flag:
    def __init__(self, name, type_name, default, category, description, source):
        self.name = name
        self.type = type_name
        self.default = default
        self.category = category
        self.description = description
        self.source = source
        self.allowed = None
        self.range = None


def strip_comments(text):
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == '"' or (c == "'" and (i == 0 or not (text[i - 1].isalnum() or text[i - 1] == "_"))):
            j = i + 1
            while j < n and text[j] != c:
                if text[j] == "\\":
                    j += 1
                j += 1
            out.append(text[i:j + 1])
            i = j + 1
        elif text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
            out.append(" ")
        else:
            out.append(c)
            i += 1
    return "".join(out)


def split_args(text, start):
    """text[start] is '('. Returns the top-level comma-separated arguments and
    the index after the matching ')'."""
    depth = 1
    i = start + 1
    n = len(text)
    current = []
    args = []
    while i < n:
        c = text[i]
        if c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                if text[j] == "\\":
                    j += 1
                j += 1
            current.append(text[i:j + 1])
            i = j + 1
            continue
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
            if depth == 0:
                args.append("".join(current).strip())
                return args, i + 1
        elif c == "," and depth == 1:
            args.append("".join(current).strip())
            current = []
            i += 1
            continue
        current.append(c)
        i += 1
    raise ValueError("unbalanced parentheses")


def parse_chain(text, i):
    """Chained builder calls after the macro: .range(..), .allowed({..}), ..."""
    chain = []
    n = len(text)
    while True:
        while i < n and text[i].isspace():
            i += 1
        match = re.match(r"\.\s*(\w+)\s*\(", text[i:])
        if not match:
            return chain
        args, i = split_args(text, i + match.end() - 1)
        chain.append((match.group(1), args))


def unescape(literal):
    return (literal.replace('\\"', '"').replace("\\n", " ").replace("\\t", " ")
            .replace("\\\\", "\\"))


def string_value(expr):
    """The concatenated value of an expression made only of string literals."""
    if not expr or STRING_RE.sub("", expr).strip():
        return None
    return "".join(unescape(m) for m in STRING_RE.findall(expr))


def number_value(token):
    token = token.strip()
    if not NUMBER_RE.match(token):
        return None
    sign = ""
    if token[0] in "+-":
        sign, token = token[0].replace("+", ""), token[1:]
    if token.lower().startswith("0x"):
        return sign + str(int(re.sub(r"[uUlL]+$", "", token), 16))
    return sign + re.sub(r"[uUlLfF]+$", "", token)


def collect_constants(files):
    """kName = <number literal> across the scanned trees, for defaults and
    ranges written as named constants. Names with several values are dropped."""
    constants = {}
    for path in files:
        text = strip_comments(read(path))
        for name, value in CONSTANT_RE.findall(text):
            number = number_value(value.strip())
            if number is None:
                continue
            if name in constants and constants[name] != number:
                constants[name] = None
            else:
                constants.setdefault(name, number)
    return {k: v for k, v in constants.items() if v is not None}


def resolve(expr, type_name, constants):
    expr = expr.strip()
    match = CAST_RE.match(expr)
    if match and not expr.startswith(("REX_", "!")):
        inner = match.group(1).strip()
        if inner:
            expr = inner
    if type_name == "string":
        value = string_value(expr)
        return None if value is None else '"%s"' % value
    if type_name == "bool":
        if expr in ("true", "false"):
            return expr
        negate = expr.startswith("!")
        symbol = expr.lstrip("!").strip()
        if symbol in PLATFORM_DEFINES:
            return str(PLATFORM_DEFINES[symbol] != negate).lower()
    number = number_value(expr)
    if number is not None:
        return number
    symbol = expr.split("::")[-1].strip()
    return constants.get(symbol)


def parse_file(path, constants, source):
    text = strip_comments(read(path))
    flags = []
    for match in MACRO_RE.finditer(text):
        type_name = TYPE_NAMES[match.group(1)]
        try:
            args, end = split_args(text, match.end() - 1)
        except ValueError:
            print("%s: unbalanced REXCVAR_DEFINE" % path, file=sys.stderr)
            continue
        if len(args) != 4:
            print("%s: REXCVAR_DEFINE with %d arguments" % (path, len(args)), file=sys.stderr)
            continue
        name, default_expr, category_expr, description_expr = args
        category = string_value(category_expr)
        description = string_value(description_expr)
        if category is None or description is None:
            print("%s: %s: category or description is not a string literal" % (path, name),
                  file=sys.stderr)
            continue
        default = resolve(default_expr, type_name, constants)
        if default is None:
            default = UNRESOLVED
        flag = Flag(name, type_name, default, category, " ".join(description.split()), source)
        for method, chain_args in parse_chain(text, end):
            if method == "allowed" and len(chain_args) == 1:
                flag.allowed = [string_value(v) or v for v in
                                split_args("(" + chain_args[0].strip()[1:-1] + ")", 0)[0]]
            elif method == "range" and len(chain_args) == 2:
                flag.range = tuple(resolve(a, "double", constants) or UNRESOLVED for a in chain_args)
        flags.append(flag)
    return flags


def read(path):
    return path.read_text(encoding="utf-8", errors="replace")


def source_files(root, skip_dirs):
    files = []
    for path in sorted(root.rglob("*")):
        if path.suffix not in SOURCE_EXTENSIONS or not path.is_file():
            continue
        parts = set(path.relative_to(root).parts[:-1])
        if parts & skip_dirs:
            continue
        files.append(path)
    return files


def scan(files, constants, source):
    flags = {}
    for path in files:
        for flag in parse_file(path, constants, source):
            if flag.name in flags:
                other = flags[flag.name]
                if (other.type, other.default, other.category) != (flag.type, flag.default, flag.category):
                    print("%s: %s is also defined in %s with other metadata" %
                          (path, flag.name, other.source), file=sys.stderr)
                continue
            flags[flag.name] = flag
    return flags


def values_for(flag, used_hints):
    if flag.allowed:
        return ", ".join("`%s`" % v for v in flag.allowed)
    if flag.range:
        note = RANGE_NOTES.get(flag.name)
        if note:
            used_hints.add(("RANGE_NOTES", flag.name))
        return "`%s`-`%s`" % flag.range + (" " + note if note else "")
    if flag.name in VALUES:
        used_hints.add(("VALUES", flag.name))
        return VALUES[flag.name]
    for prefix, value in PREFIX_VALUES.items():
        if flag.name.startswith(prefix):
            used_hints.add(("PREFIX_VALUES", prefix))
            return value
    return TYPE_FALLBACK_VALUES[flag.type]


def cell(text):
    return text.replace("|", "\\|")


def render_section(title, flags, used_hints):
    lines = ["## %s (%d flags)" % (title, len(flags)), ""]
    by_category = {}
    for flag in flags.values():
        by_category.setdefault(flag.category, []).append(flag)
    for category in sorted(by_category):
        lines += ["### %s" % category, "",
                  "| Flag | Type | Default | Values | Description |",
                  "|---|---|---|---|---|"]
        for flag in sorted(by_category[category], key=lambda f: f.name):
            lines.append("| `--%s` | %s | `%s` | %s | %s |" % (
                flag.name, flag.type, cell(flag.default), cell(values_for(flag, used_hints)),
                cell(flag.description)))
        lines.append("")
    return lines


def render(game_flags, sdk_flags):
    used_hints = set()
    lines = INTRO.strip("\n").splitlines() + [""]
    lines += render_section("Hell's Gate flags", game_flags, used_hints)
    lines += render_section("ReXGlue SDK flags (inherited by the executable)", sdk_flags, used_hints)
    for table, label in ((VALUES, "VALUES"), (RANGE_NOTES, "RANGE_NOTES"), (PREFIX_VALUES, "PREFIX_VALUES")):
        for name in sorted(table):
            if (label, name) not in used_hints:
                print("%s[%r] is unused: the flag is gone or declares its values now" % (label, name),
                      file=sys.stderr)
    return "\n".join(lines).rstrip("\n") + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--sdk", type=Path, default=REPO / "thirdparty" / "rexglue-sdk",
                        help="patched ReXGlue SDK tree (default: thirdparty/rexglue-sdk)")
    parser.add_argument("--out", type=Path, default=REPO / "docs" / "command_line_flags.md")
    parser.add_argument("--check", action="store_true",
                        help="do not write; fail if the page differs from the generated one")
    args = parser.parse_args()

    if not (args.sdk / "src").is_dir():
        sys.exit("SDK not found at %s: run setup.ps1 / setup.sh first" % args.sdk)

    game_files = source_files(REPO / "src", SKIP_DIRS)
    sdk_files = source_files(args.sdk / "src", SKIP_DIRS | SDK_SKIP_DIRS)
    constants = collect_constants(game_files + sdk_files + source_files(args.sdk / "include", SKIP_DIRS))

    game_flags = scan(game_files, constants, "src")
    sdk_flags = scan(sdk_files, constants, "SDK")
    for name in list(sdk_flags):
        if name in game_flags:
            print("%s is defined both in src/ and in the SDK" % name, file=sys.stderr)
            del sdk_flags[name]

    page = render(game_flags, sdk_flags)
    if args.check:
        current = args.out.read_text(encoding="utf-8") if args.out.exists() else ""
        if current != page:
            sys.exit("%s is out of date: run tools/gen_flags_doc.py" % args.out)
        print("%s is up to date" % args.out)
        return
    args.out.write_text(page, encoding="utf-8", newline="\n")
    print("wrote %s: %d game flags, %d SDK flags" % (args.out, len(game_flags), len(sdk_flags)))


INTRO = r"""
# Command-line flags

Every option is passed as `--name=value` to `dantes_inferno.exe`. The launcher
builds this command line from `dantes_inferno.toml` (see
`DisplayOptions.BuildLaunchArguments` in
`launcher/DantesInferno.Shared/DisplayOptions.cs`); the game itself does not
read that file, so when running the executable directly the flags are the
only way to configure it.

Two flags matter more than the rest:

- `--game_data_root` is required. The SDK locks it in before any config
  file is read, so it must be a flag (see `docs/linux-cross-compile.md`).
- `--vsync=true` is what caps the frame rate. The game paces to its vblank,
  which follows `--video_mode_refresh_rate`; without vsync the renderer runs
  unlocked while gameplay stays at the configured rate.

## What the launcher passes

With the default settings (native renderer, the display's own resolution,
SMAA on, shadow scale 2, fullscreen, 120 FPS) on a 1920x1080 display:

```
dantes_inferno.exe --game_data_root="<install>\game" --renderer=dante
  --dante_resolution=1920x1080 --dante_smaa=true --dante_shadow_scale=2
  --video_mode_refresh_rate=120 --vsync=true --fullscreen=true
  --input_backend=sdl --log_level=info --log_flush_interval=2 --glyph_family=auto
  --keybind_a=Space --keybind_b=F --keybind_x=LMB ... --keybind_start=Escape
```

The frame rate is the configured one limited to what the display can show,
so a 60 Hz monitor gets `--video_mode_refresh_rate=60`. Every `--keybind_*`
flag is passed with the binding from the launcher's Controls tab, so the
launcher's defaults (`Space`, `F`, `LMB`, `RMB`, `Shift`, `Ctrl`, ...) replace
the SDK defaults listed in the table below.

Added only when the matching setting is on:

| Flag | When |
|---|---|
| `--fullscreen_exclusive=WIDTHxHEIGHT` | Fullscreen, and the render resolution is larger than the display's. The display switches to that mode (or the closest one it has) instead of borderless fullscreen |
| `--ultrawide_target_aspect=RATIO` | Aspect ratio set to something other than Auto: `1.3333` (4:3), `1.7778` (16:9), `2.3704` (21:9) or `3.5556` (32:9) |
| `--show_fps_overlay=true` | FPS overlay enabled |
| `--anisotropic_override=N` | Anisotropic filtering chosen in the launcher; N is the launcher's level (`1`, `2`, `4`, `8` or `16`). Not passed for Default |
| `--user_language=N` | Game language other than English (`1`) |
| `--log_level=debug` | Detailed logging; replaces `--log_level=info` |

### Fallback renderer

Setting `renderer_override = "xenos"` in `dantes_inferno.toml` (a
troubleshooting option the launcher does not show) replaces `--renderer=dante`
and the `--dante_*` flags with the SDK's D3D12 renderer:

```
--renderer=xenos --render_target_path_d3d12=rov --native_render_scale=auto
```

Everything else (aspect ratio, refresh rate, vsync, fullscreen, input,
logging, glyphs, keybinds) is passed the same way.

## The tables

The **Values** column comes from the cvar's own `.allowed()` / `.range()`
where it declares one; otherwise from the code that reads it, or just the
type. Out-of-range values are rejected or clamped by the cvar system without
a visible error.

The SDK tables describe the SDK as patched by `patches/sdk/`, which is what
the executable is built with: flags such as `--fullscreen_exclusive` and
`--gpu_host_rendering` exist only there. The `rexglue` code generator's own
flags are left out; the game never registers them.

This file is generated by `tools/gen_flags_doc.py` from `src/` and
`thirdparty/rexglue-sdk` (`python tools/gen_flags_doc.py`, after `setup.ps1`
or `setup.sh`); edit the script, not the tables.
"""


if __name__ == "__main__":
    main()
