"""Compute the one-way fingerprint that locates a function at runtime.

A fingerprint is FNV-1a 64 over the first N bytes of a function (from the
module's .pdata function table), with N grown until it is unique among all
functions of the module. Only the hash and N go into source code; the bytes
themselves never leave the user's machine. Must match proxy/sites.cpp.

    python -I tools/fingerprint.py <module path> <function rva> [<rva> ...]
"""
import struct
import sys

import pefile

FNV_OFFSET, FNV_PRIME = 0xCBF29CE484222325, 0x100000001B3


def fnv1a64(b):
    h = FNV_OFFSET
    for x in b:
        h = ((h ^ x) * FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    return h


def function_starts(pe, data):
    exc = pe.OPTIONAL_HEADER.DATA_DIRECTORY[3]
    return sorted({struct.unpack_from("<I", data, off)[0]
                   for off in range(exc.VirtualAddress, exc.VirtualAddress + exc.Size, 12)})


def main(path, rvas):
    pe = pefile.PE(path, fast_load=True)
    data = pe.get_memory_mapped_image()
    starts = function_starts(pe, data)
    for rva in rvas:
        if rva not in starts:
            print(f"0x{rva:x}: not a function start")
            continue
        for n in (16, 24, 32, 48, 64, 96, 128):
            h = fnv1a64(data[rva:rva + n])
            hits = sum(1 for s in starts if fnv1a64(data[s:s + n]) == h)
            if hits == 1:
                print(f"0x{rva:x}: {{0x{h:016x}ull, {n}}}  unique")
                break
        else:
            print(f"0x{rva:x}: not unique within 128 bytes")


if __name__ == "__main__":
    main(sys.argv[1], [int(a, 16) for a in sys.argv[2:]])
