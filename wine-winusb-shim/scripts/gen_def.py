#!/usr/bin/env python3
"""Generate a .def file: forward every original export except the ones we implement.

usage: gen_def.py <dll> <out.def> <forward-target-dll> [implemented,list,comma,sep]
"""
import re
import subprocess
import sys


def export_names(dll):
    text = subprocess.run(["objdump", "-p", dll], capture_output=True, text=True,
                          check=True).stdout
    names, in_table = [], False
    for line in text.splitlines():
        if "Ordinal/Name Pointer" in line:
            in_table = True
            continue
        if in_table:
            m = re.match(r"\s*\[\s*\d+\]\s+(\S+)\s*$", line)
            if m:
                names.append(m.group(1))
                continue
            if names:
                break
    return names


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    dll, out, target = sys.argv[1], sys.argv[2], sys.argv[3]
    implemented = [n for n in (sys.argv[4].split(",") if len(sys.argv) > 4 else []) if n]

    names = export_names(dll)
    lines = ["LIBRARY " + out.rsplit("/", 1)[-1].replace(".def", ".dll"), "EXPORTS"]
    for n in implemented:
        lines.append("    " + n)
    for n in names:
        if n in implemented:
            continue
        lines.append(f"    {n} = {target}.{n}")

    with open(out, "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"{out}: {len(names)} exports, {len(implemented)} implemented, target {target}")


if __name__ == "__main__":
    main()
