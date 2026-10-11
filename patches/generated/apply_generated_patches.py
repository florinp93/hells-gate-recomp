"""Patches the generated recompiled code in generated/default. Run after every
codegen, before building.

1. Fibers: the guest setjmp/longjmp (found by function body, so both the base
   and TU2 images work) become host setjmp/longjmp at every call site.
2. Unresolved calls: REX_FATAL traps for branches the codegen didn't connect
   become direct calls or gotos where the target is known; the rest are listed.
3. Language fallback: a language missing from the game's tables falls back to
   English.
"""

import os
import re
import sys
import glob

RECOMP_GLOB = 'dantes_inferno_recomp.*.cpp'

KNOWN_SETJMP = ('sub_82879110', 'sub_82701240')
KNOWN_LONGJMP = ('sub_82878CD0', 'sub_82700CE0')

FUNC_START_RE = re.compile(r'DEFINE_REX_FUNC\((\w+)\) \{\n')

def read(path):
    with open(path, 'r', encoding='utf-8') as f:
        return f.read()

def write(path, content):
    with open(path, 'w', encoding='utf-8') as f:
        f.write(content)

def iter_function_blocks(content):
    """Yield (name, body_start, body_end) for each DEFINE_REX_FUNC block.
    Functions end at a closing brace at column 0."""
    matches = list(FUNC_START_RE.finditer(content))
    for i, m in enumerate(matches):
        end = matches[i + 1].start() if i + 1 < len(matches) else len(content)
        yield m.group(1), m.start(), end

def is_setjmp_body(body):
    """Guest setjmp signature: saves r1 @jmp_buf+144, cr @+304, flag @+312
    into the buffer pointed to by the r3 argument."""
    return ('REX_STORE_U64(ctx.r3.u32 + 144, ctx.r1.u64)' in body
            and re.search(r'REX_STORE_U32\(ctx\.r3\.u32 \+ 312,', body)
            and re.search(r'REX_STORE_U32\(ctx\.r3\.u32 \+ 304,', body))

def is_longjmp_body(body):
    """Guest longjmp signature: restores r1 @jmp_buf+144 / r31 @+296, reads
    the flag @+312, and ends the restore path with `mr r3,r6; blr`."""
    return (re.search(r'REX_LOAD_U64\(ctx\.r\d+\.u32 \+ 144', body)
            and re.search(r'REX_LOAD_U64\(ctx\.r\d+\.u32 \+ 296', body)
            and re.search(r'REX_LOAD_U32\(ctx\.r\d+\.u32 \+ 312', body)
            and 'ctx.r3.u64 = ctx.r6.u64' in body)

def scan_generated(gen_dir):
    """Single pass: detect setjmp/longjmp function names and index files."""
    files = sorted(glob.glob(os.path.join(gen_dir, RECOMP_GLOB)))
    contents = {}
    setjmp_names = []
    longjmp_names = []
    for filepath in files:
        content = read(filepath)
        contents[filepath] = content
        for name, s, e in iter_function_blocks(content):
            body = content[s:e]
            if is_setjmp_body(body):
                setjmp_names.append(name)
            elif is_longjmp_body(body):
                longjmp_names.append(name)
    for cand in KNOWN_SETJMP:
        if cand not in setjmp_names and any(
                f'DEFINE_REX_FUNC({cand})' in c for c in contents.values()):
            setjmp_names.append(cand)
    for cand in KNOWN_LONGJMP:
        if cand not in longjmp_names and any(
                f'DEFINE_REX_FUNC({cand})' in c for c in contents.values()):
            longjmp_names.append(cand)
    return files, contents, setjmp_names, longjmp_names

def patch_setjmp_body(body, name):
    """Strip the fiber-switch bnectr gate and inject FiberSetjmp before the
    `li r3,0` return path. Returns (new_body, applied_descriptions)."""
    applied = []
    if 'FIBER-CALLBACK-STRIPPED' not in body:
        new = re.sub(
            r'\t// bnectr[^\n]*\n'
            r'\tif \(!ctx\.cr0\.eq\) \{\n'
            r'\t\tREX_CALL_INDIRECT_FUNC\(ctx\.ctr\.u32\);\n'
            r'\t\treturn;\n'
            r'\t\}\n',
            '\t// bnectr FIBER-CALLBACK-STRIPPED\n',
            body, count=1)
        if new != body:
            body = new
            applied.append(f'{name}: stripped fiber-switch bnectr gate')
        else:
            print(f"  WARNING: bnectr gate not found in {name} "
                  "(callback slot must be zeroed by the app instead)")
    if 'FiberSetjmp' not in body:
        new, n = re.subn(
            r'(REX_STORE_U32\(ctx\.r3\.u32 \+ 312, ctx\.\w+\.u32\);\n)'
            r'(\t*)// li r3,0',
            '\\g<1>\\g<2>{\n'
            '\\g<2>\tint fiber_ret = FiberSetjmp(ctx.r3.u32);\n'
            '\\g<2>\tif (fiber_ret != 0) {\n'
            '\\g<2>\t\tFiberRestoreContext(ctx, base);\n'
            '\\g<2>\t\treturn;\n'
            '\\g<2>\t}\n'
            '\\g<2>}\n'
            '\\g<2>// li r3,0',
            body, count=1)
        if n:
            body = new
            applied.append(f'{name}: FiberSetjmp before return')
        else:
            print(f"  WARNING: FiberSetjmp anchor not found in {name}")
    return body, applied

def patch_longjmp_body(body, name):
    """Replace the restore-path `mr r3,r6; blr` exit with FiberLongjmp."""
    if 'FiberLongjmp' in body:
        return body, []
    pattern = (r'(ctx\.r3\.u64 = ctx\.r6\.u64;\n)'
               r'\t// blr \s*\n'
               r'\treturn;\n'
               r'(\t*loc_\w+:)')
    new, n = re.subn(pattern,
                     '\\g<1>\tFiberLongjmp(ctx.r6.u32);\n\treturn;\n\\g<2>',
                     body, count=1)
    if n == 0:
        pattern2 = (r'(ctx\.r3\.u64 = ctx\.r6\.u64;\n)'
                    r'\t// blr \s*\n'
                    r'\treturn;')
        if len(re.findall(pattern2, body)) == 1:
            new, n = re.subn(pattern2,
                             '\\g<1>\tFiberLongjmp(ctx.r6.u32);\n\treturn;',
                             body, count=1)
    if n:
        return new, [f'{name}: FiberLongjmp instead of blr']
    print(f"  WARNING: longjmp blr anchor not found in {name}")
    return body, []

def patch_setjmp_call_sites(content, name, filepath):
    """Arm the host setjmp at EVERY `bl`/`b` site of the guest setjmp."""
    call = f'\t{name}(ctx, base);\n'
    out = []
    pos = 0
    patched = 0
    while True:
        idx = content.find(call, pos)
        if idx < 0:
            break
        line_start = content.rfind('\n', 0, idx) + 1
        prev_start = content.rfind('\n', 0, max(line_start - 1, 0)) + 1
        prev_line = content[prev_start:line_start]
        if 'g_setjmp_ctx_addr = ctx.r3.u32;' in prev_line:
            pos = idx + len(call)
            continue
        out.append(content[pos:idx])
        out.append(
            '\tg_setjmp_ctx_addr = ctx.r3.u32;\n'
            f'\t{name}(ctx, base);\n'
            '\t{\n'
            '\t\tint fiber_ret = setjmp(g_fiber_jmp_buf);\n'
            '\t\tif (fiber_ret != 0) {\n'
            '\t\t\tFiberRestoreContext(ctx, base);\n'
            '\t\t}\n'
            '\t}\n')
        patched += 1
        pos = idx + len(call)
    if patched:
        out.append(content[pos:])
        content = ''.join(out)
        print(f"  Applied: setjmp call-site arm x{patched} "
              f"({name} in {os.path.basename(filepath)})")
    return content, patched

def patch_longjmp_call_sites(content, name, filepath):
    """Append `return;` after EVERY guest longjmp call site that does not
    already return (longjmp is noreturn on real hardware)."""
    call = f'\t{name}(ctx, base);\n'
    out = []
    pos = 0
    patched = 0
    while True:
        idx = content.find(call, pos)
        if idx < 0:
            break
        after = idx + len(call)
        next_line = content[after:after + 16]
        if not next_line.startswith('\treturn;'):
            out.append(content[pos:after])
            out.append('\treturn;\n')
            patched += 1
            pos = after
        else:
            pos = after
    if patched:
        out.append(content[pos:])
        content = ''.join(out)
        print(f"  Applied: longjmp call-site return x{patched} "
              f"({name} in {os.path.basename(filepath)})")
    return content, patched

def patch_fibers(files, contents, setjmp_names, longjmp_names):
    """Phase 1: fiber/setjmp/longjmp support."""
    print("== Phase 1: fiber/setjmp/longjmp patches ==")
    if not setjmp_names:
        print("  WARNING: no guest setjmp function detected")
    if not longjmp_names:
        print("  WARNING: no guest longjmp function detected")
    print(f"  setjmp:  {', '.join(setjmp_names) or 'none'}")
    print(f"  longjmp: {', '.join(longjmp_names) or 'none'}")

    for filepath in files:
        content = contents[filepath]
        changed = False
        blocks = list(iter_function_blocks(content))
        for name, s, e in blocks:
            body = content[s:e]
            if name in setjmp_names:
                new_body, applied = patch_setjmp_body(body, name)
            elif name in longjmp_names:
                new_body, applied = patch_longjmp_body(body, name)
            else:
                continue
            if new_body != body:
                content = content[:s] + new_body + content[e:]
                changed = True
            for desc in applied:
                print(f"  Applied: {desc} ({os.path.basename(filepath)})")
        if changed:
            contents[filepath] = content
            write(filepath, content)

    total_sites = 0
    for filepath in files:
        content = contents[filepath]
        original = content
        for name in setjmp_names:
            content, n = patch_setjmp_call_sites(content, name, filepath)
            total_sites += n
        for name in longjmp_names:
            content, n = patch_longjmp_call_sites(content, name, filepath)
        if content != original:
            contents[filepath] = content
            write(filepath, content)
    if total_sites == 0 and setjmp_names:
        print("  NOTE: no unpatched setjmp call sites (already applied)")

def patch_unresolved(files, contents, gen_dir):
    """Phase 2: replace REX_FATAL unresolved call/branch traps."""
    print("== Phase 2: unresolved call/branch traps ==")
    register_file = os.path.join(gen_dir, 'dantes_inferno_register.cpp')
    if not os.path.exists(register_file):
        print(f"  WARNING: register file not found: {register_file}")
        return

    registered_funcs = {}
    for match in re.finditer(
            r'registrar->SetFunction\(0x([0-9A-Fa-f]+),\s*(\w+)\)',
            read(register_file)):
        registered_funcs[match.group(1).upper()] = match.group(2)
    print(f"  {len(registered_funcs)} registered functions")

    pattern_call = re.compile(
        r'// b 0x([0-9A-Fa-f]+)\n'
        r'\s*// FATAL: unresolved function 0x[0-9A-Fa-f]+[^\n]*\n'
        r'\s*REX_FATAL\("Unresolved call from 0x[0-9A-Fa-f]+ to '
        r'0x([0-9A-Fa-f]+)"\);\n'
        r'\s*return;')
    pattern_cond = re.compile(
        r'if \((!?)ctx\.cr(\d)\.(\w+)\) '
        r'REX_FATAL\("Unresolved branch from 0x[0-9A-Fa-f]+ to '
        r'0x([0-9A-Fa-f]+)"\);')

    patched_count = 0
    skipped = []

    for filepath in files:
        content = contents[filepath]
        decls_needed = set()

        def replace_call(m):
            nonlocal patched_count
            if m.group(1).upper() != m.group(2).upper():
                skipped.append(m.group(2).upper())
                return m.group(0)
            target = m.group(2).upper()
            if target in registered_funcs:
                func_name = registered_funcs[target]
                patched_count += 1
                decls_needed.add(func_name)
                return (f'// b 0x{target}\n'
                        f'\t{func_name}(ctx, base);\n\treturn;')
            skipped.append(target)
            return m.group(0)

        def replace_cond(m):
            nonlocal patched_count
            neg, cr_num, cr_cond = m.group(1), m.group(2), m.group(3)
            target = m.group(4).upper()
            loc_name = f'loc_{target}'
            if re.search(rf'\b{re.escape(loc_name)}\s*:', content):
                patched_count += 1
                return f'if ({neg}ctx.cr{cr_num}.{cr_cond}) goto {loc_name};'
            if target in registered_funcs:
                func_name = registered_funcs[target]
                patched_count += 1
                decls_needed.add(func_name)
                return (f'if ({neg}ctx.cr{cr_num}.{cr_cond}) '
                        f'{{ {func_name}(ctx, base); return; }}')
            skipped.append(target)
            return m.group(0)

        content = pattern_call.sub(replace_call, content)
        content = pattern_cond.sub(replace_cond, content)

        if decls_needed:
            decl_text = '\n// Forward declarations for unresolved call patches\n'
            for fn_name in sorted(decls_needed):
                decl_text += f'DECLARE_REX_FUNC({fn_name});\n'
            content = re.sub(r'(#include "[^"]+")', r'\1' + decl_text,
                             content, count=1)

        if content != contents[filepath]:
            contents[filepath] = content
            write(filepath, content)

    print(f"  Patched {patched_count} unresolved calls/branches")
    if skipped:
        unique = sorted(set(skipped))
        print(f"  SKIPPED {len(skipped)} sites ({len(unique)} unique targets) "
              "- need manifest [entrypoint.functions.*] entries + regen:")
        for t in unique:
            print(f"    {t}")

def patch_language_fallback(files, contents):
    """Phase 3: when the requested language ID is absent from the game's
    runtime tables, retry the lookup with English (id 1) so unsupported
    selections fall back to English instead of record 0 (Italian on the
    pal_it SKU)."""
    print("== Phase 3: unsupported-language fallback ==")
    anchor = 'loc_823C2504:\n'
    inject = (
        '\t{\n'
        '\t\tuint32_t lang_tbl = REX_LOAD_U32(ctx.r31.u32 + 32);\n'
        '\t\tif (lang_tbl) {\n'
        '\t\t\tif (ctx.r7.s32 < 0) {\n'
        '\t\t\t\tuint32_t cnt = REX_LOAD_U32(lang_tbl + 232);\n'
        '\t\t\t\tuint32_t arr = REX_LOAD_U32(lang_tbl + 240);\n'
        '\t\t\t\tfor (uint32_t i = 0; arr && i < cnt; ++i)\n'
        '\t\t\t\t\tif ((int32_t)REX_LOAD_U32(arr + i * 100 + 64) == 1) {\n'
        '\t\t\t\t\t\tctx.r7.u64 = i;\n'
        '\t\t\t\t\t\tbreak;\n'
        '\t\t\t\t\t}\n'
        '\t\t\t}\n'
        '\t\t\tif (ctx.r30.s32 < 0) {\n'
        '\t\t\t\tuint32_t cnt = REX_LOAD_U32(lang_tbl + 228);\n'
        '\t\t\t\tuint32_t arr = REX_LOAD_U32(lang_tbl + 236);\n'
        '\t\t\t\tfor (uint32_t i = 0; arr && i < cnt; ++i)\n'
        '\t\t\t\t\tif ((int32_t)REX_LOAD_U32(arr + i * 104 + 64) == 1) {\n'
        '\t\t\t\t\t\tctx.r30.u64 = i;\n'
        '\t\t\t\t\t\tbreak;\n'
        '\t\t\t\t\t}\n'
        '\t\t\t}\n'
        '\t\t}\n'
        '\t}\n')
    done = False
    for filepath in files:
        content = contents[filepath]
        if anchor not in content:
            continue
        if 'LANGFALLBACK' in content or inject in content:
            print(f"  already applied ({os.path.basename(filepath)})")
            done = True
            continue
        new, n = re.subn(re.escape(anchor), anchor + inject, content, count=1)
        if n:
            contents[filepath] = new
            write(filepath, new)
            print(f"  Applied: en-fallback at loc_823C2504 "
                  f"({os.path.basename(filepath)})")
            done = True
    if not done:
        print("  WARNING: loc_823C2504 anchor not found "
              "(only exists in TU2 codegen)")

def count_remaining_fatals(files, contents):
    total = 0
    for filepath in files:
        total += contents[filepath].count('REX_FATAL("Unresolved')
    return total

def main():
    project_root = os.path.dirname(os.path.dirname(
        os.path.dirname(os.path.abspath(__file__))))
    gen_dir = os.path.join(project_root, 'generated', 'default')

    if not os.path.isdir(gen_dir):
        print(f"ERROR: Generated directory not found: {gen_dir}",
              file=sys.stderr)
        sys.exit(1)

    files, contents, setjmp_names, longjmp_names = scan_generated(gen_dir)
    patch_fibers(files, contents, setjmp_names, longjmp_names)
    patch_unresolved(files, contents, gen_dir)
    patch_language_fallback(files, contents)

    remaining = count_remaining_fatals(files, contents)
    print(f"== Done. Remaining REX_FATAL unresolved traps: {remaining} ==")

if __name__ == '__main__':
    main()
