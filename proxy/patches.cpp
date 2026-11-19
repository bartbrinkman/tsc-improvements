#include "patches.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "log.h"
#include "sites.h"

namespace tscfix::patches {
namespace {

constexpr size_t kMaxSite = 16;  // longest run of instructions a site may move

struct Site {
    char name[32] = {};
    BYTE* at = nullptr;        // first moved instruction inside the engine
    size_t length = 0;         // bytes moved (>= 5 for the jump)
    BYTE original[kMaxSite] = {};
    BYTE jump[kMaxSite] = {};  // jmp rel32 to the stub, padded with int3
    BYTE* stub = nullptr;
};

std::vector<Site> g_sites;
bool g_active = false;

uint64_t Fnv1a64(const BYTE* p, size_t n) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

int RegNumber(const std::string& r) {
    static const char* names[] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                                  "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};
    for (int i = 0; i < 16; ++i)
        if (r == names[i]) return i;
    return -1;
}

// x64 encoder for the few instruction forms a recipe needs.
struct Emitter {
    std::vector<BYTE> code;
    std::vector<size_t> skips;  // rel32 fields that jump to the end of the recipe

    void Mem(int reg, int base, int32_t disp) {  // ModRM (mod=10, disp32) [+ SIB for rsp/r12]
        code.push_back(static_cast<BYTE>(0x80 | ((reg & 7) << 3) | (base & 7)));
        if ((base & 7) == 4) code.push_back(0x24);
        for (int i = 0; i < 4; ++i) code.push_back(static_cast<BYTE>(disp >> (8 * i)));
    }
    void Load(int dst, int base, int32_t disp, BYTE opcode) {  // 8B = mov, 8D = lea
        code.push_back(static_cast<BYTE>(0x48 | (dst >= 8 ? 4 : 0) | (base >= 8 ? 1 : 0)));
        code.push_back(opcode);
        Mem(dst, base, disp);
    }
    void Prefetch(int base, int32_t disp) {
        if (base >= 8) code.push_back(0x41);
        code.push_back(0x0F);
        code.push_back(0x18);
        Mem(1, base, disp);  // /1 = prefetcht0
    }
    void SkipIf(int a, int b, BYTE jcc) {  // cmp a, b; jcc <end>
        code.push_back(static_cast<BYTE>(0x48 | (b >= 8 ? 4 : 0) | (a >= 8 ? 1 : 0)));
        code.push_back(0x39);
        code.push_back(static_cast<BYTE>(0xC0 | ((b & 7) << 3) | (a & 7)));
        code.push_back(0x0F);
        code.push_back(jcc);
        skips.push_back(code.size());
        for (int i = 0; i < 4; ++i) code.push_back(0);
    }
    void Finish() {
        for (size_t at : skips) {
            int32_t rel = static_cast<int32_t>(code.size() - (at + 4));
            memcpy(&code[at], &rel, 4);
        }
    }
};

// Parses "[base+disp]" / "[base-disp]" / "[base]".
bool ParseMem(const std::string& s, int& base, int32_t& disp) {
    if (s.size() < 5 || s.front() != '[' || s.back() != ']') return false;
    std::string in = s.substr(1, s.size() - 2);
    size_t op = in.find_first_of("+-");
    base = RegNumber(in.substr(0, op));
    disp = 0;
    if (op != std::string::npos) {
        long v = strtol(in.c_str() + op + 1, nullptr, 0);
        disp = static_cast<int32_t>(in[op] == '-' ? -v : v);
    }
    return base >= 0;
}

bool Compile(const std::string& recipe, Emitter& e) {
    size_t pos = 0;
    while (pos <= recipe.size()) {
        size_t end = recipe.find(';', pos);
        if (end == std::string::npos) end = recipe.size();
        std::string step = recipe.substr(pos, end - pos);
        pos = end + 1;
        if (step.empty()) continue;
        int base = 0;
        int32_t disp = 0;
        if (step.rfind("pf", 0) == 0) {
            if (!ParseMem(step.substr(2), base, disp)) return false;
            e.Prefetch(base, disp);
        } else if (step[0] == '?') {
            size_t eq = step.find("=="), ge = step.find(">=");
            size_t at = eq != std::string::npos ? eq : ge;
            if (at == std::string::npos) return false;
            int a = RegNumber(step.substr(1, at - 1)), b = RegNumber(step.substr(at + 2));
            if (a < 0 || b < 0) return false;
            e.SkipIf(a, b, eq != std::string::npos ? 0x84 : 0x83);  // je / jae
        } else {
            size_t eq = step.find('=');
            if (eq == std::string::npos) return false;
            int dst = RegNumber(step.substr(0, eq));
            bool address = step[eq + 1] == '&';
            if (dst < 0 || !ParseMem(step.substr(eq + (address ? 2 : 1)), base, disp)) return false;
            e.Load(dst, base, disp, address ? 0x8D : 0x8B);
        }
    }
    e.Finish();
    return true;
}

// Executable memory within rel32 reach of `target`.
BYTE* AllocNear(BYTE* target, size_t size) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t gran = si.dwAllocationGranularity;
    uintptr_t start = (reinterpret_cast<uintptr_t>(target) & ~(gran - 1));
    for (uintptr_t delta = gran; delta < 0x7FF00000; delta += gran) {
        for (uintptr_t candidate : {start - delta, start + delta}) {
            void* p = VirtualAlloc(reinterpret_cast<void*>(candidate), size, MEM_COMMIT | MEM_RESERVE,
                                   PAGE_EXECUTE_READWRITE);
            if (p) return static_cast<BYTE*>(p);
        }
    }
    return nullptr;
}

void WriteCode(BYTE* at, const BYTE* bytes, size_t n) {
    DWORD old = 0;
    VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old);
    memcpy(at, bytes, n);
    VirtualProtect(at, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, n);
}

bool Prepare(HMODULE engine, const char* name, const std::string& spec, Site& site) {
    // <fp hash>,<fp length>,<offset>,<length>,<bytes hash>,<recipe>
    std::vector<std::string> f;
    size_t pos = 0;
    for (int i = 0; i < 5; ++i) {
        size_t c = spec.find(',', pos);
        if (c == std::string::npos) return false;
        f.push_back(spec.substr(pos, c - pos));
        pos = c + 1;
    }
    f.push_back(spec.substr(pos));
    sites::Fingerprint fp{strtoull(f[0].c_str(), nullptr, 16), strtoull(f[1].c_str(), nullptr, 0)};
    auto fn = static_cast<BYTE*>(sites::Find(engine, fp));
    size_t offset = strtoull(f[2].c_str(), nullptr, 16), length = strtoull(f[3].c_str(), nullptr, 0);
    if (!fn) {
        Log("patches: %s: function not found", name);
        return false;
    }
    if (length < 5 || length > kMaxSite) return false;
    site.at = fn + offset;
    site.length = length;
    if (Fnv1a64(site.at, length) != strtoull(f[4].c_str(), nullptr, 16)) {
        Log("patches: %s: bytes differ (game updated?); skipped", name);
        return false;
    }
    Emitter e;
    if (!Compile(f[5], e)) {
        Log("patches: %s: bad recipe", name);
        return false;
    }
    site.stub = AllocNear(site.at, 4096);
    if (!site.stub) return false;
    // stub = recipe, moved instructions, jmp back
    std::vector<BYTE> stub = e.code;
    stub.insert(stub.end(), site.at, site.at + length);
    stub.push_back(0xE9);
    int32_t back = static_cast<int32_t>((site.at + length) - (site.stub + stub.size() + 4));
    for (int i = 0; i < 4; ++i) stub.push_back(static_cast<BYTE>(back >> (8 * i)));
    memcpy(site.stub, stub.data(), stub.size());
    FlushInstructionCache(GetCurrentProcess(), site.stub, stub.size());

    memcpy(site.original, site.at, length);
    memset(site.jump, 0xCC, sizeof site.jump);
    site.jump[0] = 0xE9;
    int32_t to = static_cast<int32_t>(site.stub - (site.at + 5));
    memcpy(site.jump + 1, &to, 4);
    strncpy_s(site.name, name, _TRUNCATE);
    return true;
}

}  // namespace

int Install(HMODULE engine, const char* iniPath) {
    std::vector<char> keys(8192);
    DWORD n = GetPrivateProfileStringA("patches", nullptr, "", keys.data(), static_cast<DWORD>(keys.size()), iniPath);
    for (const char* k = keys.data(); k < keys.data() + n && *k; k += strlen(k) + 1) {
        if (strcmp(k, "enable") == 0) continue;
        char spec[512];
        GetPrivateProfileStringA("patches", k, "", spec, sizeof spec, iniPath);
        Site site;
        if (Prepare(engine, k, spec, site)) g_sites.push_back(site);
    }
    Log("patches: %zu site(s) prepared", g_sites.size());
    return static_cast<int>(g_sites.size());
}

void SetActive(bool active) {
    if (active == g_active) return;
    g_active = active;
    for (Site& s : g_sites) WriteCode(s.at, active ? s.jump : s.original, s.length);
}

}  // namespace tscfix::patches
