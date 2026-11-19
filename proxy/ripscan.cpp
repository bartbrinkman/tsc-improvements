#include "ripscan.h"

#include <cstring>

#include "hde/hde64.h"

namespace tscfix::ripscan {
namespace {

// The instruction reads or writes rax without naming it in ModRM.reg.
bool UsesRaxImplicitly(const hde64s& h) {
    if (h.opcode == 0x0F) return h.opcode2 == 0xB0 || h.opcode2 == 0xB1 || h.opcode2 == 0xA2;  // cmpxchg, cpuid
    if (h.opcode == 0xF6 || h.opcode == 0xF7) return h.modrm_reg >= 4;  // mul, imul, div, idiv
    return false;
}

// Re-encodes the instruction at p (decoded as h, RIP-relative) to address [base] with no
// displacement. Returns false if it should not be redirected.
bool Reencode(const uint8_t* p, const hde64s& h, Ref& r) {
    if (h.p_seg == 0x64 || h.p_seg == 0x65) return false;
    if (h.opcode == 0xFF && h.modrm_reg >= 2 && h.modrm_reg <= 6) return false;  // call / jmp / push
    if (h.opcode == 0x8F) return false;                                          // pop
    if (h.rex_b) return false;
    size_t imm = (h.flags & F_IMM64) ? 8 : (h.flags & F_IMM32) ? 4 : (h.flags & F_IMM16) ? 2 : (h.flags & F_IMM8) ? 1 : 0;
    size_t dispAt = h.len - imm - 4;
    size_t modrmAt = dispAt - 1;
    if (p[modrmAt] != h.modrm) return false;
    // ModRM.reg names a register unless it extends the opcode (immediate groups, shifts, unary ops).
    bool regIsOperand = true;
    switch (h.opcode) {
        case 0x80: case 0x81: case 0x83: case 0x8F: case 0xC0: case 0xC1: case 0xC6: case 0xC7:
        case 0xD0: case 0xD1: case 0xD2: case 0xD3: case 0xF6: case 0xF7: case 0xFE: case 0xFF:
            regIsOperand = false;
            break;
        case 0x0F:
            regIsOperand = !(h.opcode2 <= 0x01 || (h.opcode2 >= 0x18 && h.opcode2 <= 0x1F) || h.opcode2 == 0xAE ||
                             h.opcode2 == 0xBA || h.opcode2 == 0xC7 ||
                             // SSE: ModRM.reg is an xmm register
                             (h.opcode2 >= 0x10 && h.opcode2 <= 0x17) || (h.opcode2 >= 0x28 && h.opcode2 <= 0x2F) ||
                             (h.opcode2 >= 0x50 && h.opcode2 <= 0x7F) || h.opcode2 == 0xC2 || h.opcode2 == 0xC6 ||
                             h.opcode2 >= 0xD0 || h.opcode2 == 0x38 || h.opcode2 == 0x3A);
            break;
    }
    unsigned reg = regIsOperand ? (h.modrm_reg | (h.rex_r ? 8 : 0)) : 16;
    bool usesRcx = h.opcode == 0xD2 || h.opcode == 0xD3;  // shift by cl
    uint8_t base = 0;  // rax, unless the instruction uses it
    if (reg == 0 || UsesRaxImplicitly(h)) base = 1;
    if (reg == base || (base == 1 && usesRcx)) base = 2;
    r.scratch = reg == 11 ? 10 : 11;  // (reg 16: not a register operand)
    r.base = base;
    size_t n = 0;
    memcpy(r.reencoded, p, modrmAt);
    n = modrmAt;
    r.reencoded[n++] = static_cast<uint8_t>((h.modrm_reg << 3) | base);  // mod 00, rm = base
    memcpy(r.reencoded + n, p + dispAt + 4, imm);
    n += imm;
    r.newLength = static_cast<uint8_t>(n);
    return h.len >= 5;  // the site becomes a 5-byte call
}

}  // namespace

std::vector<Ref> Find(HMODULE module, uintptr_t lo, uintptr_t hi, size_t* skipped) {
    std::vector<Ref> out;
    size_t skip = 0;
    auto base = reinterpret_cast<const uint8_t*>(module);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    auto funcs = reinterpret_cast<const RUNTIME_FUNCTION*>(base + dir.VirtualAddress);
    size_t count = dir.Size / sizeof(RUNTIME_FUNCTION);
    for (size_t f = 0; f < count; ++f) {
        const uint8_t* p = base + funcs[f].BeginAddress;
        const uint8_t* end = base + funcs[f].EndAddress;
        while (p < end) {
            hde64s h;
            hde64_disasm(p, &h);
            if (h.flags & F_ERROR || !h.len) break;
            if (h.flags & F_MODRM && h.modrm_mod == 0 && h.modrm_rm == 5) {
                size_t imm = (h.flags & F_IMM64) ? 8 : (h.flags & F_IMM32) ? 4 : (h.flags & F_IMM16) ? 2 : (h.flags & F_IMM8) ? 1 : 0;
                int32_t disp;
                memcpy(&disp, p + h.len - imm - 4, 4);
                uintptr_t target = reinterpret_cast<uintptr_t>(p) + h.len + disp;
                if (target >= lo && target < hi) {
                    Ref r = {};
                    r.at = reinterpret_cast<uintptr_t>(p);
                    r.length = h.len;
                    r.target = target;
                    if (Reencode(p, h, r)) out.push_back(r);
                    else ++skip;
                }
            }
            p += h.len;
        }
    }
    if (skipped) *skipped = skip;
    return out;
}

}  // namespace tscfix::ripscan
