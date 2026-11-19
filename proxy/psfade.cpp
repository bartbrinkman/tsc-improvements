#include "psfade.h"

#include <cstring>

namespace tscfix::psfade {
namespace {

// Direct3D 9 shader byte code (documented in the Windows driver kit, "Shader code format").
constexpr uint32_t kVersionPs30 = 0xFFFF0300, kEnd = 0x0000FFFF;
constexpr uint32_t kOpMask = 0xFFFF, kComment = 0xFFFE;
enum Op : uint32_t { kMov = 1, kMad = 4, kMul = 5, kFrc = 19, kDcl = 31, kDefB = 47, kDefI = 48, kTexkill = 65, kDef = 81, kDp2add = 90 };
enum RegType : uint32_t { kTemp = 0, kConst = 2, kMisc = 17 };

uint32_t RegTypeOf(uint32_t token) { return ((token >> 28) & 7) | ((token >> 8) & 0x18); }
uint32_t RegNumber(uint32_t token) { return token & 0x7ff; }
uint32_t Reg(RegType type, uint32_t number) { return 0x80000000u | (number & 0x7ff) | ((type & 7u) << 28) | ((type & 0x18u) << 8); }
uint32_t Dest(RegType type, uint32_t number, uint32_t mask = 0xF) { return Reg(type, number) | (mask << 16); }
constexpr uint32_t kXyzw = 0xE4, kXxxx = 0x00, kYyyy = 0x55, kZzzz = 0xAA, kWwww = 0xFF;
uint32_t Src(RegType type, uint32_t number, uint32_t swizzle = kXyzw) { return Reg(type, number) | (swizzle << 16); }
uint32_t Instruction(Op op, uint32_t operands) { return op | (operands << 24); }
uint32_t FloatBits(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

// Tokens of the instruction at p (the opcode token included).
size_t InstructionLength(const uint32_t* p) {
    uint32_t op = *p & kOpMask;
    if (op == kComment) return 1 + ((*p >> 16) & 0x7fff);
    return 1 + ((*p >> 24) & 0xf);
}

}  // namespace

size_t Length(const uint32_t* code, size_t limit) {
    size_t i = 1;
    while (i < limit) {
        if (code[i] == kEnd) return i + 1;
        i += InstructionLength(code + i);
    }
    return 0;
}

Result Patch(const uint32_t* code, std::vector<uint32_t>* out) {
    if (code[0] != kVersionPs30) return Result::NotModel3;
    size_t total = Length(code, 1 << 20);
    if (!total) return Result::Malformed;

    // One pass: the highest temporary register, use of our two constant registers, a vPos
    // declaration, and the place of the first instruction after the declarations.
    int maxTemp = -1;
    bool hasVpos = false, registerUsed = false, inHeader = true;
    size_t insertAt = 0;
    for (size_t i = 1; i + 1 < total;) {
        const uint32_t* p = code + i;
        uint32_t op = *p & kOpMask;
        size_t len = InstructionLength(p);
        if (i + len > total) return Result::Malformed;
        bool header = op == kComment || op == kDcl || op == kDef || op == kDefI || op == kDefB;
        if (inHeader && !header) inHeader = false, insertAt = i;
        if (op != kComment) {
            // Which operand tokens are registers: all of them, but for the constants of a def and
            // the usage token of a dcl.
            size_t first = 1, last = len;
            if (op == kDef || op == kDefI || op == kDefB) last = 2;
            if (op == kDcl) first = 2;
            for (size_t k = first; k < last; ++k) {
                uint32_t token = p[k];
                if (!(token & 0x80000000u)) continue;
                uint32_t type = RegTypeOf(token), number = RegNumber(token);
                if (type == kTemp && static_cast<int>(number) > maxTemp) maxTemp = static_cast<int>(number);
                if (type == kConst && (number == kFadeRegister || number == kNoiseRegister)) registerUsed = true;
                if (type == kConst && (token & 0x2000)) registerUsed = true;  // indexed constants: any register can be read
                if (op == kDcl && type == kMisc && number == 0) hasVpos = true;
            }
        }
        i += len;
    }
    if (!insertAt) insertAt = total - 1;  // only declarations: insert before the end token
    if (registerUsed) return Result::RegisterInUse;
    if (maxTemp >= 31) return Result::NoFreeTemp;
    uint32_t t = static_cast<uint32_t>(maxTemp + 1);

    std::vector<uint32_t> add;
    if (!hasVpos) {
        add.push_back(Instruction(kDcl, 2));
        add.push_back(0x80000000u);          // usage token (position, index 0)
        add.push_back(Dest(kMisc, 0, 0x3));  // vPos.xy
    }
    add.push_back(Instruction(kDef, 5));
    add.push_back(Dest(kConst, kNoiseRegister));
    add.push_back(FloatBits(0.06711056f)), add.push_back(FloatBits(0.00583715f)), add.push_back(FloatBits(52.9829189f)), add.push_back(0);
    add.push_back(Instruction(kDp2add, 4));
    add.push_back(Dest(kTemp, t)), add.push_back(Src(kMisc, 0, 0x44)), add.push_back(Src(kConst, kNoiseRegister)), add.push_back(Src(kConst, kNoiseRegister, kWwww));  // vPos.xyxy: only x and y are declared
    add.push_back(Instruction(kFrc, 2));
    add.push_back(Dest(kTemp, t)), add.push_back(Src(kTemp, t));
    add.push_back(Instruction(kMul, 3));
    add.push_back(Dest(kTemp, t)), add.push_back(Src(kTemp, t)), add.push_back(Src(kConst, kNoiseRegister, kZzzz));
    add.push_back(Instruction(kFrc, 2));
    add.push_back(Dest(kTemp, t)), add.push_back(Src(kTemp, t));
    add.push_back(Instruction(kMad, 4));
    add.push_back(Dest(kTemp, t)), add.push_back(Src(kTemp, t)), add.push_back(Src(kConst, kFadeRegister, kYyyy)), add.push_back(Src(kConst, kFadeRegister, kXxxx));
    add.push_back(Instruction(kTexkill, 1));
    add.push_back(Dest(kTemp, t));

    out->clear();
    out->reserve(total + add.size());
    out->insert(out->end(), code, code + insertAt);
    out->insert(out->end(), add.begin(), add.end());
    out->insert(out->end(), code + insertAt, code + total);
    return Result::Patched;
}

}  // namespace tscfix::psfade
