"""List which functions reference given strings / imports.

    python -I re/xref.py Touhou6/th06nc.exe "replay/th6_%.2d.rpy" import:srand ...

A bare argument is matched against ASCII and UTF-16 strings in the image
(exact match); `import:NAME` targets an IAT slot; `rva:HEX` targets an RVA.
"""
import os
import sys

sys.path.append(os.path.dirname(os.path.abspath(__file__)))
from pe import PE  # noqa: E402


def find_string(pe, s):
    out = []
    for enc, term in (("ascii", b"\0"), ("utf-16le", b"\0\0")):
        try:
            needle = s.encode(enc)
        except UnicodeEncodeError:
            continue
        start = 0
        while True:
            i = pe.data.find(needle + term, start)
            if i < 0:
                break
            start = i + 1
            # require a terminator or alignment before it so we hit whole strings only
            if enc == "ascii" and i and pe.data[i - 1] not in (0,):
                continue
            rva = pe.off2rva(i)
            if rva is not None:
                out.append(rva)
    return out


def main():
    pe = PE(sys.argv[1])
    targets = {}
    for arg in sys.argv[2:]:
        if arg.startswith("import:"):
            name = arg[7:]
            if name in pe.imports:
                targets[pe.imports[name]] = arg
            else:
                print("no such import:", name)
        elif arg.startswith("rva:"):
            targets[int(arg[4:], 16)] = arg
        else:
            rvas = find_string(pe, arg)
            if not rvas:
                print("string not found:", repr(arg))
            for r in rvas:
                targets[r] = repr(arg)
    hits = pe.rip_xrefs(targets.keys())
    for t in sorted(targets):
        print("%s @ %08X" % (targets[t], t))
        for x in hits[t]:
            f = pe.func_of(x)
            where = "func %08X-%08X (+0x%X)" % (f[0], f[1], x - f[0]) if f else "no unwind entry"
            print("    ref at %08X  %s" % (x, where))
        if not hits[t]:
            print("    (no direct rip-relative refs)")


main()
