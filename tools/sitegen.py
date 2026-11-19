"""Generate proxy/engine_sites.h: engine locations as one-way fingerprints plus offsets.

    python -I tools/sitegen.py <module path> <names file> <out header>

The names file (kept local, gitignored) has one "Name rva" pair per line. Each rva becomes a
sites::Site: the fingerprint (FNV-1a 64 over the first N bytes, N grown until unique) of the
function that contains it plus the offset into that function, or, for data, a fingerprinted
function with an instruction that addresses the data RIP-relative. Only hashes and offsets are
written; the bytes themselves never leave the user's machine. Must match proxy/sites.cpp.
"""
import struct
import sys

import capstone
import pefile

FNV_OFFSET, FNV_PRIME = 0xCBF29CE484222325, 0x100000001B3
LENGTHS = (16, 24, 32, 48, 64, 96, 128)


def fnv1a64(b):
    h = FNV_OFFSET
    for x in b:
        h = ((h ^ x) * FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    return h


class Module:
    def __init__(self, path):
        self.pe = pefile.PE(path, fast_load=True)
        self.img = self.pe.get_memory_mapped_image()
        exc = self.pe.OPTIONAL_HEADER.DATA_DIRECTORY[3]
        self.funcs = [struct.unpack_from("<III", self.img, off)
                      for off in range(exc.VirtualAddress, exc.VirtualAddress + exc.Size, 12)]
        self.starts = sorted({f[0] for f in self.funcs})
        text = next(s for s in self.pe.sections if s.Name.rstrip(b"\0") == b".text")
        self.text = (text.VirtualAddress, text.VirtualAddress + text.Misc_VirtualSize)
        self.counts = {}  # length -> {hash: count}
        self.fp_cache = {}

    def primary(self, rva):
        """Start of the function containing rva, following chained unwind info to its primary."""
        entry = None
        for b, e, u in self.funcs:
            if b <= rva < e:
                entry = (b, e, u)
                break
        if entry is None:
            return None
        for _ in range(32):
            u = entry[2]
            flags = self.img[u] >> 3
            if not flags & 4:  # UNW_FLAG_CHAININFO
                return entry[0]
            codes = self.img[u + 2]
            chained = u + 4 + 2 * ((codes + 1) & ~1)
            entry = struct.unpack_from("<III", self.img, chained)
        return entry[0]

    def fingerprint(self, start):
        if start in self.fp_cache:
            return self.fp_cache[start]
        for n in LENGTHS:
            if n not in self.counts:
                c = {}
                for s in self.starts:
                    h = fnv1a64(self.img[s:s + n])
                    c[h] = c.get(h, 0) + 1
                self.counts[n] = c
            h = fnv1a64(self.img[start:start + n])
            if self.counts[n][h] == 1:
                self.fp_cache[start] = (h, n)
                return h, n
        self.fp_cache[start] = None
        return None

    def vtable_slots(self):
        """Function rva -> (class RTTI name, slot) for every primary vtable (COL offset 0)."""
        base = self.pe.OPTIONAL_HEADER.ImageBase
        rdata = next(s for s in self.pe.sections if s.Name.rstrip(b"\0") == b".rdata")
        lo, hi = rdata.VirtualAddress, rdata.VirtualAddress + rdata.Misc_VirtualSize
        out = {}
        for p in range(lo, hi - 8, 8):
            col = struct.unpack_from("<Q", self.img, p)[0] - base
            if not (lo <= col < hi - 0x18):
                continue
            sig, off, _, td = struct.unpack_from("<IIII", self.img, col)
            if sig != 1 or not (0 < td < len(self.img) - 0x20):
                continue
            name = self.img[td + 0x10:td + 0x10 + 200].split(b"\0")[0]
            if not name.startswith(b".?AV"):
                continue
            for slot in range(0, 512):
                f = struct.unpack_from("<Q", self.img, p + 8 + 8 * slot)[0] - base
                if not (self.text[0] <= f < self.text[1]):
                    break
                out.setdefault(f, (name.decode(), slot, off))
        return out

    def data_refs(self, targets):
        """For each data rva, RIP-relative instructions in .text that address it."""
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        found = {t: [] for t in targets}
        lo, hi = self.text
        for i in range(lo, hi - 4):
            disp = struct.unpack_from("<i", self.img, i)[0]
            for extra in (0, 1, 2, 4):
                t = i + 4 + extra + disp
                if t not in found or len(found[t]) >= 8:
                    continue
                for back in range(1, 8):
                    for ins in md.disasm(self.img[i - back:i - back + 16], i - back, 1):
                        direct = ins.mnemonic in ("call", "jmp") and ins.op_str == hex(t)
                        if ins.address + ins.size == i + 4 + extra and ("rip" in ins.op_str or direct):
                            found[t].append((ins.address, back, ins.size))
        return found


def main(path, names_path, out_path):
    m = Module(path)
    names = []
    for line in open(names_path, encoding="utf-8"):
        line = line.split("#")[0].strip()
        if line:
            name, rva = line.split()
            names.append((name, int(rva, 16)))
    def direct(rva):
        if not (m.text[0] <= rva < m.text[1]):
            return None
        start = m.primary(rva)
        fp = m.fingerprint(start) if start is not None else None
        return (fp, start) if fp else None
    # Code whose own function has no unique fingerprint is reached through a reference to it
    # (a call or RIP-relative operand) from a function that has one, like data.
    indirect = [rva for _, rva in names if direct(rva) is None]
    refs = m.data_refs(set(indirect)) if indirect else {}
    rows = []
    vt = None
    for name, rva in names:
        d = direct(rva)
        if d:
            (h, n), start = d
            rows.append((name, h, n, 0, 0, 0, rva - start))
            continue
        for addr, disp_at, length in refs.get(rva, []):
            start = m.primary(addr)
            fp = m.fingerprint(start) if start is not None else None
            if fp:
                rows.append((name, fp[0], fp[1], 1, disp_at, length, addr - start))
                break
        else:
            # Reached only through a vtable: the class (by RTTI name) and the slot.
            if vt is None:
                vt = m.vtable_slots()
            if rva not in vt:
                sys.exit(f"{name}: no fingerprintable reference")
            cls, slot, sub = vt[rva]
            rows.append((name, sub, 0, 2, 0, 0, slot, cls))  # kind 2: hash holds the sub-object offset
    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write("#pragma once\n// Generated by tools/sitegen.py: engine locations as one-way fingerprints plus offsets\n")
        f.write("// (docs/IP-POLICY.md). Resolved at start-up by sites::ResolveAll.\n")
        f.write('#include <cstdint>\n\n#include "sites.h"\n\nnamespace tscfix::engine {\n\nenum Site : uint16_t {\n')
        for row in rows:
            f.write(f"    {row[0]},\n")
        f.write("    kSiteCount\n};\n\ninline constexpr sites::Site kSites[kSiteCount] = {\n")
        for row in rows:
            name, h, n, kind, disp_at, length, off = row[:7]
            cls = f'"{row[7]}"' if len(row) > 7 else "nullptr"
            f.write(f"    {{0x{h:016x}ull, {n}, {kind}, {disp_at}, {length}, 0x{off:x}, {cls}}},  // {name}\n")
        f.write("};\n\n")
        f.write("// The resolved locations (offsets from the module base; 0 = not found), resolved once.\n")
        f.write("inline const uintptr_t* Resolved(HMODULE engine, size_t* found = nullptr) {\n")
        f.write("    static uintptr_t got[kSiteCount];\n    static size_t n = sites::ResolveAll(engine, kSites, kSiteCount, got);\n")
        f.write("    if (found) *found = n;\n    return got;\n}\n\n")
        f.write("}  // namespace tscfix::engine\n")
    print(f"{len(rows)} sites -> {out_path}")


if __name__ == "__main__":
    main(*sys.argv[1:4])
