"""Drop func_mappings entries outside the image's code range.

The manifest carries a few "unresolved_target" stubs at TU2-only addresses
(0x8287xxxx). Codegen on the base-game image emits them too; the runtime then
rejects them (outside the module) and Runtime::Setup fails with C0000001.
They have no code, so removing them is safe. Run after codegen.
"""
import os
import re
import sys

gen = sys.argv[1] if len(sys.argv) > 1 else os.path.join("generated", "default")
pch = open(os.path.join(gen, "dantes_inferno_pch.h"), encoding="utf-8").read()
base = int(re.search(r"#define REX_CODE_BASE (0x[0-9A-Fa-f]+)", pch).group(1), 16)
size = int(re.search(r"#define REX_CODE_SIZE (0x[0-9A-Fa-f]+)", pch).group(1), 16)
end = base + size

dropped = []
patterns = {
    "dantes_inferno_init.cpp": re.compile(r"^\s*\{ (0x[0-9A-Fa-f]+), (\w+) \},\s*$"),
    "dantes_inferno_register.cpp": re.compile(r"^\s*registrar->SetFunction\((0x[0-9A-Fa-f]+), (\w+)\);\s*$"),
}
for name, entry in patterns.items():
    path = os.path.join(gen, name)
    if not os.path.exists(path):
        continue
    lines = open(path, encoding="utf-8").read().split("\n")
    kept = []
    for ln in lines:
        m = entry.match(ln)
        if m and int(m.group(1), 16) != 0 and not (base <= int(m.group(1), 16) < end):
            dropped.append(m.group(2))
            continue
        kept.append(ln)
    if len(kept) != len(lines):
        open(path, "w", encoding="utf-8").write("\n".join(kept))
print(f"code range {base:08X}-{end:08X}: dropped {len(dropped)} out-of-range mappings "
      + (", ".join(dropped) if dropped else ""))
