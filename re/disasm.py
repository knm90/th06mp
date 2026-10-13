"""Annotated disassembly of one function (or address range) of the game exe.

    python -I re/disasm.py Touhou6/th06nc.exe 45B90            # whole function
    python -I re/disasm.py Touhou6/th06nc.exe 45B90 --len 200  # byte range

Addresses are RVAs. RIP-relative operands are resolved and annotated with
the string / import / symbol they point at. Names come from symbols.txt.
"""
import argparse
import os
import sys

sys.path.append(os.path.dirname(os.path.abspath(__file__)))
import capstone  # noqa: E402
from capstone import x86 as cx  # noqa: E402
from pe import PE  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))


def load_symbols(path=None):
    """symbols.txt: `RVA name  # comment` per line. Returns {rva: name}."""
    path = path or os.path.join(HERE, "symbols.txt")
    out = {}
    if os.path.exists(path):
        for line in open(path, encoding="utf-8"):
            line = line.split("#", 1)[0].split()
            if len(line) >= 2:
                out[int(line[0], 16)] = line[1]
    return out


class Annotator:
    def __init__(self, pe, symbols=None):
        self.pe = pe
        self.sym = load_symbols() if symbols is None else symbols
        self.iat = {rva: name for name, rva in pe.imports.items()}

    def string_at(self, rva):
        raw = self.pe.read(rva, 96)
        a = raw.split(b"\0", 1)[0]
        if len(a) >= 3 and all(32 <= c < 127 or c in (9, 10, 13) for c in a):
            return '"%s"' % a.decode("ascii").replace("\n", "\\n")
        w = raw[:raw.find(b"\0\0\0") + 1] if b"\0\0\0" in raw else b""
        if len(w) >= 6 and len(w) % 2 == 0:
            try:
                s = w.decode("utf-16le")
                if all(32 <= ord(c) < 0x3000 for c in s):
                    return 'L"%s"' % s
            except UnicodeDecodeError:
                pass
        return None

    def name(self, rva):
        if rva in self.sym:
            return self.sym[rva]
        if rva in self.iat:
            return "imp:" + self.iat[rva]
        sec = self.pe.section_of(rva)
        if sec == ".rdata":
            s = self.string_at(rva)
            if s:
                return s
        return None

    def describe(self, rva):
        n = self.name(rva)
        sec = self.pe.section_of(rva) or "?"
        return "%s:%08X%s" % (sec, rva, " " + n if n else "")


def make_cs():
    cs = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    cs.detail = True
    return cs


def rip_target(insn):
    """RVA a RIP-relative memory operand points at, or None."""
    for op in insn.operands:
        if op.type == cx.X86_OP_MEM and op.mem.base == cx.X86_REG_RIP:
            return insn.address + insn.size + op.mem.disp
    return None


def branch_target(insn):
    if insn.group(cx.X86_GRP_CALL) or insn.group(cx.X86_GRP_JUMP):
        op = insn.operands[0]
        if op.type == cx.X86_OP_IMM:
            return op.imm
    return None


def disassemble(pe, start, end, ann=None, out=sys.stdout):
    ann = ann or Annotator(pe)
    cs = make_cs()
    code = pe.read(start, end - start)
    last = start
    for insn in cs.disasm(code, start):
        note = ""
        t = rip_target(insn)
        if t is not None:
            note = ann.describe(t)
        else:
            b = branch_target(insn)
            if b is not None and not (start <= b < end):
                note = ann.name(b) or ""
        out.write("%08X  %-9s %-40s%s\n" % (
            insn.address, insn.mnemonic, insn.op_str, ("  ; " + note) if note else ""))
        last = insn.address + insn.size
    if last < end:
        out.write("%08X  (decode stopped, %d bytes left)\n" % (last, end - last))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("rva", type=lambda s: int(s, 16))
    ap.add_argument("--len", type=lambda s: int(s, 0), default=None)
    args = ap.parse_args()
    pe = PE(args.exe)
    if args.len is not None:
        start, end = args.rva, args.rva + args.len
    else:
        f = pe.func_of(args.rva)
        if not f:
            sys.exit("no unwind entry covers %X; pass --len" % args.rva)
        start, end = f
    ann = Annotator(pe)
    print("; %08X-%08X %s" % (start, end, ann.sym.get(start, "")))
    disassemble(pe, start, end, ann)


if __name__ == "__main__":
    main()
