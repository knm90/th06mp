"""Minimal PE32+ reader used by the analysis scripts in this folder.

Everything is addressed by RVA (offset from the image base), because the
game is built with ASLR enabled and its absolute addresses change per run.
"""
import bisect
import struct


class PE:
    def __init__(self, path):
        self.path = path
        self.data = open(path, "rb").read()
        d = self.data
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        nsec = struct.unpack_from("<H", d, pe + 6)[0]
        optsz = struct.unpack_from("<H", d, pe + 20)[0]
        opt = pe + 24
        if struct.unpack_from("<H", d, opt)[0] != 0x20B:
            raise ValueError("not a PE32+ image")
        self.image_base = struct.unpack_from("<Q", d, opt + 24)[0]
        self.entry = struct.unpack_from("<I", d, opt + 16)[0]
        self.dirs = [struct.unpack_from("<II", d, opt + 112 + i * 8) for i in range(16)]
        self.sections = []
        o = opt + optsz
        for _ in range(nsec):
            name = d[o:o + 8].rstrip(b"\0").decode()
            vsize, va, rsize, rptr = struct.unpack_from("<IIII", d, o + 8)
            self.sections.append((name, va, vsize, rptr, rsize))
            o += 40
        self._funcs = None
        self._imports = None

    # -- address translation -------------------------------------------------
    def section(self, name):
        for s in self.sections:
            if s[0] == name:
                return s
        raise KeyError(name)

    def section_of(self, rva):
        for name, va, vsize, rptr, rsize in self.sections:
            if va <= rva < va + max(vsize, rsize):
                return name
        return None

    def rva2off(self, rva):
        for name, va, vsize, rptr, rsize in self.sections:
            if va <= rva < va + rsize:
                return rva - va + rptr
        return None

    def off2rva(self, off):
        for name, va, vsize, rptr, rsize in self.sections:
            if rptr <= off < rptr + rsize:
                return off - rptr + va
        return None

    def read(self, rva, n):
        off = self.rva2off(rva)
        if off is None:
            return b"\0" * n  # uninitialised (.bss-like) part of a section
        return self.data[off:off + n]

    def u32(self, rva):
        return struct.unpack("<I", self.read(rva, 4))[0]

    def u64(self, rva):
        return struct.unpack("<Q", self.read(rva, 8))[0]

    def cstr(self, rva, limit=512):
        raw = self.read(rva, limit)
        return raw.split(b"\0", 1)[0].decode("latin-1")

    # -- functions (from the x64 exception directory) ------------------------
    @property
    def funcs(self):
        """Sorted list of (begin_rva, end_rva) for every function with unwind info."""
        if self._funcs is None:
            rva, size = self.dirs[3]
            off = self.rva2off(rva)
            out = []
            for i in range(size // 12):
                b, e, _ = struct.unpack_from("<III", self.data, off + i * 12)
                if b:
                    out.append((b, e))
            out.sort()
            self._funcs = out
            self._starts = [f[0] for f in out]
        return self._funcs

    def func_of(self, rva):
        """Function (begin, end) containing rva, or None (leaf functions have no entry)."""
        self.funcs
        i = bisect.bisect_right(self._starts, rva) - 1
        if i >= 0 and self._funcs[i][0] <= rva < self._funcs[i][1]:
            return self._funcs[i]
        return None

    # -- imports -------------------------------------------------------------
    @property
    def imports(self):
        """Dict of import name -> RVA of its IAT slot."""
        if self._imports is None:
            out = {}
            off = self.rva2off(self.dirs[1][0])
            while True:
                oft, _, _, name_rva, ft = struct.unpack_from("<IIIII", self.data, off)
                if not name_rva:
                    break
                dll = self.cstr(name_rva)
                thunk = oft or ft
                i = 0
                while True:
                    v = self.u64(thunk + i * 8)
                    if not v:
                        break
                    if v >> 63:
                        name = "%s!#%d" % (dll, v & 0xFFFF)
                    else:
                        name = self.cstr((v & 0x7FFFFFFF) + 2)
                    out[name] = ft + i * 8
                    i += 1
                off += 20
            self._imports = out
        return self._imports

    # -- cross references ----------------------------------------------------
    def rip_xrefs(self, targets):
        """Find RIP-relative references from .text to any RVA in `targets`.

        Returns {target_rva: [rva of the 4-byte displacement, ...]}. This is a
        raw scan for rel32 values (no disassembly), so it only sees operands
        with no immediate after the displacement - which covers lea/mov/call/jmp.
        """
        name, va, vsize, rptr, rsize = self.section(".text")
        text = self.data[rptr:rptr + min(vsize, rsize)]
        want = set(targets)
        hits = {t: [] for t in want}
        for k in range(4):
            mv = memoryview(text)[k:]
            mv = mv[:len(mv) // 4 * 4].cast("i")
            base = va + k + 4
            for i, disp in enumerate(mv):
                t = base + i * 4 + disp
                if t in want:
                    hits[t].append(va + k + i * 4)
        return hits
