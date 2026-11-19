#include "safecounts.h"

#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "log.h"

namespace tscfix::safecounts {
namespace {

const uint8_t* g_textLo = nullptr;
const uint8_t* g_textHi = nullptr;

bool InText(const uint8_t* f, size_t room) { return f >= g_textLo && f < g_textHi - room; }
bool Padding(uint8_t b) { return b == 0xcc || b == 0x90; }

// The place of the count: [rcx+disp8] or [rcx+disp32].
struct Place {
    int width;  // 1 or 4 displacement bytes
    int32_t disp;
    bool operator==(const Place& o) const { return width == o.width && disp == o.disp; }
};

// Appends the operand bytes "modrm disp" for [rcx+disp] with the given reg field.
size_t Operand(uint8_t* out, int reg, Place p) {
    out[0] = static_cast<uint8_t>((p.width == 1 ? 0x41 : 0x81) | (reg << 3));
    memcpy(out + 1, &p.disp, p.width);
    return 1 + p.width;
}

// ---- AddRef: "inc dword [rcx+N]; ret" -------------------------------------------------------

bool ReadAddRef(const uint8_t* f, Place* p, size_t* length) {
    if (f[0] != 0xff) return false;
    if (f[1] == 0x41 && f[3] == 0xc3) return *p = {1, static_cast<int8_t>(f[2])}, *length = 4, true;
    if (f[1] == 0x81 && f[6] == 0xc3) {
        p->width = 4;
        memcpy(&p->disp, f + 2, 4);
        *length = 7;
        return true;
    }
    return false;
}

// "sub rcx, imm; jmp rel32" that jumps to `target`. Returns its length, or 0.
size_t ThunkTo(const uint8_t* f, const uint8_t* target) {
    size_t sub;
    if (f[0] == 0x48 && f[1] == 0x83 && f[2] == 0xe9 && f[4] == 0xe9) sub = 4;
    else if (f[0] == 0x48 && f[1] == 0x81 && f[2] == 0xe9 && f[7] == 0xe9) sub = 7;
    else return 0;
    int32_t rel;
    memcpy(&rel, f + sub + 1, 4);
    return f + sub + 5 + rel == target ? sub + 5 : 0;
}

// The new bytes of an AddRef (and of the thunk right behind it, if there is no padding byte).
// Returns the number of bytes to write at f, or 0 when the layout is not a known one.
size_t SafeAddRef(const uint8_t* f, Place p, size_t length, uint8_t* out) {
    uint8_t inc[8];
    inc[0] = 0xf0, inc[1] = 0xff;
    size_t n = 2 + Operand(inc + 2, 0, p);
    inc[n++] = 0xc3;  // lock inc dword [rcx+N]; ret
    if (Padding(f[length])) {
        memcpy(out, inc, n);
        return n;
    }
    // No padding: a thunk for a second base class follows and jumps back to f. Then
    //   f:   jmp short L      f+4: sub rcx, imm (the thunk's entry stays where it is)      L: lock inc; ret
    // so the thunk falls through into the safe add. The block has the same length as before.
    if (length != 4) return 0;
    size_t thunk = ThunkTo(f + 4, f);
    if (!thunk) return 0;
    size_t sub = thunk - 5;
    out[0] = 0xeb, out[1] = static_cast<uint8_t>(2 + sub), out[2] = out[3] = 0xcc;
    memcpy(out + 4, f + 4, sub);
    memcpy(out + 4 + sub, inc, n);  // n == 5 here, the length of the thunk's jmp
    return 4 + sub + n;
}

// ---- Release ---------------------------------------------------------------------------------

// The end of every Release that destroys: "mov rax,[rcx]; mov edx,1; jmp [rax+slot*8]; ret".
// A class whose counted part is not at the object's start first moves rcx back and skips the
// destroy for a null result: "add rcx,imm8; je ret" in front of it.
size_t ReadDestroy(const uint8_t* f) {
    size_t adjust = 0;
    if (f[0] == 0x48 && f[1] == 0x83 && f[2] == 0xc1 && f[4] == 0x74) adjust = 6;
    const uint8_t* g = f + adjust;
    static const uint8_t head[] = {0x48, 0x8b, 0x01, 0xba, 0x01, 0x00, 0x00, 0x00, 0x48, 0xff};
    if (memcmp(g, head, sizeof head)) return 0;
    size_t rest = 0;
    if (g[10] == 0x60 && g[12] == 0xc3) rest = 13;       // jmp [rax+disp8]
    else if (g[10] == 0x20 && g[11] == 0xc3) rest = 12;  // jmp [rax]
    if (!rest) return 0;
    if (adjust && f[5] != rest - 1) return 0;  // the "je" must go to the final ret
    return adjust + rest;
}

// The new bytes of a Release. Returns the number of bytes to write at f, or 0 when the function
// is not one of the known shapes or the safe form does not fit.
size_t SafeRelease(const uint8_t* f, Place p, uint8_t* out) {
    uint8_t operand[5];
    size_t d = Operand(operand, 0, p);  // reg field filled in per use below
    auto at = [&](int reg) {
        operand[0] = static_cast<uint8_t>((operand[0] & 0xc7) | (reg << 3));
        return operand;
    };
    // Shape "dec dword [rcx+N]; ret" (no destroy).
    if (f[0] == 0xff && !memcmp(f + 1, at(1), d) && f[1 + d] == 0xc3 && Padding(f[2 + d])) {
        out[0] = 0xf0, out[1] = 0xff;
        memcpy(out + 2, at(1), d);
        out[2 + d] = 0xc3;
        return 3 + d;
    }
    // Shape "sub dword [rcx+N],1; jnz ret; destroy": the lock prefix moves everything one byte.
    if (f[0] == 0x83 && !memcmp(f + 1, at(5), d) && f[1 + d] == 0x01 && f[2 + d] == 0x75) {
        size_t destroy = ReadDestroy(f + 4 + d);
        if (!destroy || f[3 + d] != destroy - 1 || !Padding(f[4 + d + destroy])) return 0;
        out[0] = 0xf0;
        memcpy(out + 1, f, 4 + d + destroy);
        return 5 + d + destroy;
    }
    // Shape "dec dword [rcx+N]; cmp dword [rcx+N],0; jg ret; destroy": the locked dec sets the flags
    // that "jg" reads, so the compare becomes a no-op of the same length.
    if (f[0] == 0xff && !memcmp(f + 1, at(1), d) && f[1 + d] == 0x83 && !memcmp(f + 2 + d, at(7), d) && f[2 + 2 * d] == 0x00 &&
        f[3 + 2 * d] == 0x7f && p.width == 1) {
        size_t destroy = ReadDestroy(f + 5 + 2 * d);
        if (!destroy || f[4 + 2 * d] != destroy - 1) return 0;
        size_t n = 0;
        out[n++] = 0xf0, out[n++] = 0xff;
        memcpy(out + n, at(1), d), n += d;       // lock dec [rcx+N]: 4 bytes where "dec" had 3
        out[n++] = 0x0f, out[n++] = 0x1f, out[n++] = 0x00;  // nop, 3 bytes where "cmp" had 4
        memcpy(out + n, f + 3 + 2 * d, 2 + destroy), n += 2 + destroy;
        return n;
    }
    // Shape "mov eax,[rcx+N]; test eax,eax; jz destroy; sub eax,1; mov [rcx+N],eax; jnz ret; destroy".
    // A count of zero, or a count that becomes zero, destroys the object.
    size_t k = 0;
    if (f[k] != 0x8b || memcmp(f + k + 1, at(0), d)) return 0;
    k += 1 + d;
    static const uint8_t mid[] = {0x85, 0xc0, 0x74};
    if (memcmp(f + k, mid, 3)) return 0;
    k += 4;  // the jz distance is checked through the total length below
    static const uint8_t dec[] = {0x83, 0xe8, 0x01, 0x89};
    if (memcmp(f + k, dec, 4) || memcmp(f + k + 4, at(0), d) || f[k + 4 + d] != 0x75) return 0;
    k += 4 + d + 2;
    size_t destroy = ReadDestroy(f + k);
    if (!destroy || f[k - 1] != destroy - 1) return 0;  // jnz must go to the final ret
    size_t oldLength = k + destroy;
    //   lock sub [rcx+N],1; jz destroy; jnc ret; lock inc [rcx+N]; destroy
    // Zero after the subtract: the last user is gone. A borrow: the count was zero already; the
    // engine destroys then too, with the count still zero, so the count is put back first.
    size_t n = 0;
    out[n++] = 0xf0, out[n++] = 0x83;
    memcpy(out + n, at(5), d), n += d;
    out[n++] = 0x01;
    out[n++] = 0x74, out[n++] = static_cast<uint8_t>(2 + 2 + d);
    out[n++] = 0x73, out[n++] = static_cast<uint8_t>(2 + d + destroy - 1);
    out[n++] = 0xf0, out[n++] = 0xff;
    memcpy(out + n, at(0), d), n += d;
    memcpy(out + n, f + k, destroy), n += destroy;
    return n <= oldLength ? n : 0;
}

bool Write(uint8_t* at, const uint8_t* bytes, size_t n) {
    DWORD old;
    if (!VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(at, bytes, n);
    VirtualProtect(at, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, n);
    return true;
}

}  // namespace

bool Install(HMODULE engine) {
    uint8_t* base = reinterpret_cast<uint8_t*>(engine);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    const uint8_t* dataLo = nullptr;
    const uint8_t* dataHi = nullptr;
    for (int s = 0; s < nt->FileHeader.NumberOfSections; ++s) {
        if (!memcmp(sec[s].Name, ".text", 5)) g_textLo = base + sec[s].VirtualAddress, g_textHi = g_textLo + sec[s].Misc.VirtualSize;
        if (!memcmp(sec[s].Name, ".rdata", 6)) dataLo = base + sec[s].VirtualAddress, dataHi = dataLo + sec[s].Misc.VirtualSize;
    }
    if (!g_textLo || !dataLo) return false;

    // Every vtable slot that holds a plain AddRef; the next slot is that class's Release.
    std::unordered_map<uint8_t*, std::vector<uint8_t*>> releasesOf;
    for (const uint8_t* p = dataLo; p + 16 <= dataHi; p += 8) {
        uint8_t* f = *reinterpret_cast<uint8_t* const*>(p);
        Place place;
        size_t length;
        if (!InText(f, 32) || !ReadAddRef(f, &place, &length)) continue;
        releasesOf[f].push_back(*reinterpret_cast<uint8_t* const*>(p + 8));
    }

    struct Patch {
        uint8_t* at;
        uint8_t bytes[48];
        size_t n;
    };
    std::vector<Patch> patches;
    int classes = 0, skipped = 0;
    for (auto& [addRef, releases] : releasesOf) {
        Place place;
        size_t length;
        ReadAddRef(addRef, &place, &length);
        std::vector<Patch> mine;
        Patch a{addRef, {}, 0};
        a.n = SafeAddRef(addRef, place, length, a.bytes);
        bool ok = a.n != 0;
        mine.push_back(a);
        for (uint8_t* release : releases) {
            Patch r{release, {}, 0};
            if (ok && InText(release, 64)) r.n = SafeRelease(release, place, r.bytes);
            ok = ok && r.n != 0;
            mine.push_back(r);
        }
        if (!ok) {
            ++skipped;
            continue;
        }
        ++classes;
        patches.insert(patches.end(), mine.begin(), mine.end());
    }
    int written = 0;
    for (const Patch& p : patches)
        if (memcmp(p.at, p.bytes, p.n) == 0 || Write(p.at, p.bytes, p.n)) ++written;  // (a Release shared by two AddRefs is written once)
    Log("fix: safe_counts: %d plain user counts made thread-safe (%d functions changed in memory); %d left as they are (unknown code shape)",
        classes, written, skipped);
    return classes > 0;
}

}  // namespace tscfix::safecounts
