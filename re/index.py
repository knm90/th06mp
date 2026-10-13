"""Whole-image cross-reference index for the game exe, with a small query CLI.

    python -I re/index.py Touhou6/th06nc.exe build
    python -I re/index.py Touhou6/th06nc.exe refs  4F1440 [4F1480]   # who touches this data range
    python -I re/index.py Touhou6/th06nc.exe callers 3BE80           # who calls / jumps to this
    python -I re/index.py Touhou6/th06nc.exe calls 3BE80             # what this function calls
    python -I re/index.py Touhou6/th06nc.exe info 3BE80

All addresses are RVAs. The index is cached next to this script in .cache/.
"""
import os
import pickle
import struct
import sys

sys.path.append(os.path.dirname(os.path.abspath(__file__)))
import capstone  # noqa: E402
from capstone import x86 as cx  # noqa: E402
from disasm import Annotator, make_cs  # noqa: E402
from pe import PE  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
CACHE = os.path.join(HERE, ".cache")


def root_map(pe):
    """Map every .pdata chunk start to the start of the function it belongs to.

    MSVC splits one function into several unwind chunks; the extra chunks carry
    UNW_FLAG_CHAININFO and point back at their parent RUNTIME_FUNCTION.
    """
    rva, size = pe.dirs[3]
    off = pe.rva2off(rva)
    parent = {}
    for i in range(size // 12):
        begin, end, unwind = struct.unpack_from("<III", pe.data, off + i * 12)
        if not begin:
            continue
        uo = pe.rva2off(unwind)
        ver_flags, _, ncodes, _ = struct.unpack_from("<BBBB", pe.data, uo)
        if (ver_flags >> 3) & 4:
            chain = uo + 4 + ((ncodes + 1) & ~1) * 2
            parent[begin] = struct.unpack_from("<I", pe.data, chain)[0]
    roots = {}
    for begin, _ in pe.funcs:
        r = begin
        while r in parent and parent[r] != r:
            r = parent[r]
        roots[begin] = r
    return roots


def build(pe):
    cs = make_cs()
    roots = root_map(pe)
    data_refs = []   # (insn_rva, target_rva, access, size, chunk_begin)
    code_refs = []   # (insn_rva, target_rva, kind, chunk_begin)  kind: c=call j=jmp
    for begin, end in pe.funcs:
        code = pe.read(begin, end - begin)
        for insn in cs.disasm(code, begin):
            is_call = insn.group(cx.X86_GRP_CALL)
            is_jump = insn.group(cx.X86_GRP_JUMP)
            for op in insn.operands:
                if op.type == cx.X86_OP_MEM and op.mem.base == cx.X86_REG_RIP:
                    t = insn.address + insn.size + op.mem.disp
                    if insn.id == cx.X86_INS_LEA:
                        acc = "lea"
                    elif is_call or is_jump:
                        acc = "x"
                    else:
                        acc = ("r" if op.access & capstone.CS_AC_READ else "") + \
                              ("w" if op.access & capstone.CS_AC_WRITE else "")
                    data_refs.append((insn.address, t, acc or "?", op.size, begin))
                elif op.type == cx.X86_OP_IMM and (is_call or is_jump):
                    t = op.imm
                    if not (begin <= t < end):
                        code_refs.append((insn.address, t, "c" if is_call else "j", begin))
    return {"roots": roots, "data_refs": data_refs, "code_refs": code_refs}


def load(pe, rebuild=False):
    os.makedirs(CACHE, exist_ok=True)
    path = os.path.join(CACHE, os.path.basename(pe.path) + ".idx")
    stamp = (os.path.getsize(pe.path), int(os.path.getmtime(pe.path)))
    if not rebuild and os.path.exists(path):
        with open(path, "rb") as f:
            idx = pickle.load(f)
        if idx.get("stamp") == stamp:
            return idx
    idx = build(pe)
    idx["stamp"] = stamp
    with open(path, "wb") as f:
        pickle.dump(idx, f)
    return idx


class Index:
    def __init__(self, exe, rebuild=False):
        self.pe = PE(exe)
        self.ann = Annotator(self.pe)
        self.idx = load(self.pe, rebuild)
        self.roots = self.idx["roots"]

    def root(self, chunk):
        return self.roots.get(chunk, chunk)

    def fname(self, rva):
        return self.ann.sym.get(rva, "sub_%X" % rva)

    def refs(self, lo, hi=None):
        hi = hi if hi is not None else lo + 1
        return [r for r in self.idx["data_refs"] if lo <= r[1] < hi]

    def callers(self, target):
        return [r for r in self.idx["code_refs"] if r[1] == target]

    def calls(self, func):
        return [r for r in self.idx["code_refs"] if self.root(r[3]) == func]

    def data_of(self, func):
        return [r for r in self.idx["data_refs"] if self.root(r[4]) == func]


def main():
    exe, cmd = sys.argv[1], sys.argv[2]
    args = [int(a, 16) for a in sys.argv[3:]]
    ix = Index(exe, rebuild=(cmd == "build"))
    if cmd == "build":
        print("%d data refs, %d code refs, %d chunks" % (
            len(ix.idx["data_refs"]), len(ix.idx["code_refs"]), len(ix.pe.funcs)))
    elif cmd == "refs":
        by_func = {}
        for insn, t, acc, size, chunk in ix.refs(*args):
            by_func.setdefault(ix.root(chunk), []).append((insn, t, acc, size))
        for f in sorted(by_func):
            print("%s (%08X): %d ref(s)" % (ix.fname(f), f, len(by_func[f])))
            for insn, t, acc, size in by_func[f]:
                print("    %08X  %-3s %d  -> %08X" % (insn, acc, size, t))
    elif cmd == "callers":
        for insn, t, kind, chunk in ix.callers(args[0]):
            f = ix.root(chunk)
            print("%08X  %s  in %s (%08X)" % (insn, "call" if kind == "c" else "jmp ", ix.fname(f), f))
    elif cmd == "calls":
        seen = {}
        for insn, t, kind, chunk in ix.calls(args[0]):
            seen.setdefault(t, []).append(insn)
        for t in sorted(seen):
            print("%08X  %-28s x%d" % (t, ix.ann.name(t) or "", len(seen[t])))
    elif cmd == "info":
        f = args[0]
        print("callers: %d   callees: %d   data refs: %d" % (
            len(ix.callers(f)), len({r[1] for r in ix.calls(f)}), len(ix.data_of(f))))
        for insn, t, acc, size, chunk in ix.data_of(f):
            n = ix.ann.name(t)
            if n:
                print("    %08X  %-3s -> %08X %s" % (insn, acc, t, n))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
