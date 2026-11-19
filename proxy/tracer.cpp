// Research only (gitignored). Single-steps one engine call with the trap flag and records
// every non-stack memory access (instruction address + effective address).
#include "tracer.h"

#include <intrin.h>
#include <psapi.h>
#include <winternl.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "hde/hde64.h"
#include "log.h"

namespace tscfix::tracer {
namespace {

// ea: 0 = none, 1 = undecodable, 2.. = address. before/after: the 8 bytes at ea when the
// instruction is about to run and once it has run (for telling idempotent writes apart).
struct Record { uint64_t rip; uint64_t ea; uint64_t before; uint64_t after; };
constexpr size_t kMaxRecords = size_t(40) << 20;  // 1.25 GiB of records at most

Record* g_buf = nullptr;

// Type tagging: at every entry of a function of the traced module, the function and its first
// argument (rcx, `this` for member functions) with that object's vtable; and for every memory
// access within the current frame's object, the frame's function, the object's vtable and the
// offset (index into g_buf for before / after). Turned into names offline (tools/name_types.py).
struct Entry { uint64_t fn; uint64_t rcx; uint64_t vtable; uint64_t record; };
struct Field { uint64_t fn; uint64_t vtable; uint32_t offset; uint32_t pad; uint64_t record; };
constexpr size_t kMaxEntries = size_t(8) << 20, kMaxFields = size_t(32) << 20;
Entry* g_entries = nullptr;
Field* g_fields = nullptr;
size_t g_entryCount = 0, g_fieldCount = 0;
uintptr_t g_modBase = 0, g_modEnd = 0;  // the module whose functions are tagged
uint8_t* g_starts = nullptr;            // bitmap of its function starts (.pdata)
struct Frame { uint64_t rsp; uint64_t fn; uint64_t self; uint64_t vtable; };
Frame g_frames[512];
int g_depth = 0;
bool IsStart(uintptr_t a) {
    if (!g_starts || a < g_modBase || a >= g_modEnd) return false;
    size_t o = a - g_modBase;
    return (g_starts[o >> 3] >> (o & 7)) & 1;
}
void BuildStarts(HMODULE m) {
    auto* b = reinterpret_cast<uint8_t*>(m);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(b + reinterpret_cast<IMAGE_DOS_HEADER*>(b)->e_lfanew);
    g_modBase = reinterpret_cast<uintptr_t>(b);
    g_modEnd = g_modBase + nt->OptionalHeader.SizeOfImage;
    g_starts = static_cast<uint8_t*>(VirtualAlloc(nullptr, nt->OptionalHeader.SizeOfImage / 8 + 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!g_starts) return;
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    auto* f = reinterpret_cast<const RUNTIME_FUNCTION*>(b + dir.VirtualAddress);
    for (size_t i = 0; i < dir.Size / sizeof(RUNTIME_FUNCTION); ++i) {
        DWORD o = f[i].BeginAddress;
        g_starts[o >> 3] |= static_cast<uint8_t>(1 << (o & 7));
    }
}
size_t g_count = 0;
uint64_t g_steps = 0;
volatile bool g_armed = false;
volatile int g_phase = 0;  // 0 idle, 1 stepping (waiting to leave our module), 2 recording, 3 done
DWORD g_tid = 0;
}  // namespace
// The calling thread is being single-stepped (its exceptions include the tracer's own guarded
// reads, which are not crashes).
bool Tracing() { return (g_phase == 1 || g_phase == 2) && GetCurrentThreadId() == g_tid; }
namespace {
uintptr_t g_selfBase = 0, g_selfEnd = 0;
uintptr_t g_stackLow = 0, g_stackHigh = 0;
uintptr_t g_startRsp = 0;
PVOID g_veh = nullptr;

uint64_t Reg(const CONTEXT* c, int r) {
    switch (r) {
        case 0: return c->Rax;  case 1: return c->Rcx;  case 2: return c->Rdx;  case 3: return c->Rbx;
        case 4: return c->Rsp;  case 5: return c->Rbp;  case 6: return c->Rsi;  case 7: return c->Rdi;
        case 8: return c->R8;   case 9: return c->R9;   case 10: return c->R10; case 11: return c->R11;
        case 12: return c->R12; case 13: return c->R13; case 14: return c->R14; default: return c->R15;
    }
}

// Effective address of the instruction's memory operand, 0 if it has none, 1 if unknown.
uint64_t Peek(uint64_t ea);
uint64_t EffectiveAddress(const CONTEXT* c) {
    hde64s h;
    __try {
        hde64_disasm(reinterpret_cast<const void*>(c->Rip), &h);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
    if (h.flags & F_ERROR) return 1;
    if (h.p_seg == 0x64 || h.p_seg == 0x65) return 0;  // fs/gs: thread-local
    // String instructions write through rdi.
    uint8_t op = h.opcode;
    if (!(h.flags & F_MODRM)) {
        if (op == 0xA4 || op == 0xA5 || op == 0xAA || op == 0xAB) return c->Rdi;
        return 0;
    }
    if (h.modrm_mod == 3) return 0;
    int64_t disp = (h.flags & F_DISP8) ? static_cast<int8_t>(h.disp.disp8)
                 : (h.flags & F_DISP32) ? static_cast<int32_t>(h.disp.disp32) : 0;
    if (h.modrm_mod == 0 && h.modrm_rm == 5) return c->Rip + h.len + disp;  // rip-relative
    uint64_t ea = disp;
    if (h.modrm_rm == 4) {  // SIB
        int index = h.sib_index | (h.rex_x << 3);
        if (index != 4) ea += Reg(c, index) << h.sib_scale;
        if (!(h.modrm_mod == 0 && h.sib_base == 5)) ea += Reg(c, h.sib_base | (h.rex_b << 3));
    } else {
        ea += Reg(c, h.modrm_rm | (h.rex_b << 3));
    }
    return ea;
}

Record* g_pendingAfter = nullptr;

uint64_t Peek(uint64_t ea) {
    __try {
        return *reinterpret_cast<const volatile uint64_t*>(ea);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

LONG CALLBACK StepHandler(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || g_phase == 0 || GetCurrentThreadId() != g_tid)
        return EXCEPTION_CONTINUE_SEARCH;
    if (g_phase == 3) return EXCEPTION_CONTINUE_EXECUTION;  // leftover step after the trace: swallow
    CONTEXT* c = ep->ContextRecord;
    uintptr_t rip = c->Rip;
    bool inSelf = rip >= g_selfBase && rip < g_selfEnd;
    // Entry: the first instruction outside our module is the traced function (or its hook
    // trampoline); its stack pointer marks the frame. Above it again = the call returned.
    if (g_phase == 2 && c->Rsp > g_startRsp) {
        g_phase = 3;
        return EXCEPTION_CONTINUE_EXECUTION;  // trap flag not re-set: stepping ends
    }
    if (g_phase == 1 && !inSelf) {
        g_phase = 2;
        g_startRsp = c->Rsp;
    }
    if (g_phase == 2) {
        ++g_steps;
        if (g_entries) {
            while (g_depth > 0 && c->Rsp > g_frames[g_depth - 1].rsp) --g_depth;  // returned
            if (IsStart(rip)) {
                uint64_t vt = c->Rcx > 0x10000 ? Peek(c->Rcx) : 0;
                if (vt < g_modBase || vt >= g_modEnd) vt = 0;  // only objects with an engine vtable
                if (g_entryCount < kMaxEntries) g_entries[g_entryCount++] = {rip, c->Rcx, vt, g_count};
                if (g_depth < 512) g_frames[g_depth++] = {c->Rsp, rip, c->Rcx, vt};
            }
        }
        if (g_pendingAfter) {  // the previous instruction has run now
            g_pendingAfter->after = Peek(g_pendingAfter->ea);
            g_pendingAfter = nullptr;
        }
        uint64_t ea = EffectiveAddress(c);
        bool stack = ea >= g_stackLow && ea < g_stackHigh;
        if (ea != 0 && !stack && g_count < kMaxRecords) {
            Record& r = g_buf[g_count++];
            r = {rip, ea, ea > 1 ? Peek(ea) : 0, 0};
            if (ea > 1) g_pendingAfter = &r;
            if (g_fields && g_depth > 0 && g_fieldCount < kMaxFields) {
                const Frame& fr = g_frames[g_depth - 1];
                if (fr.vtable && ea >= fr.self && ea < fr.self + 0x4000)
                    g_fields[g_fieldCount++] = {fr.fn, fr.vtable, static_cast<uint32_t>(ea - fr.self), 0, g_count - 1};
            }
        }
    }
    c->EFlags |= 0x100;
    return EXCEPTION_CONTINUE_EXECUTION;
}

void WriteOutput(const char* tag) {
    char path[MAX_PATH], file[64];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    sprintf_s(file, "tscfix-trace%s%s.bin", *tag ? "-" : "", tag);
    strcpy_s(slash + 1, MAX_PATH - (slash + 1 - path), file);
    FILE* f = nullptr;
    if (!fopen_s(&f, path, "wb") && f) {
        fwrite(g_buf, sizeof(Record), g_count, f);
        fclose(f);
    }
    if (g_entries) {
        sprintf_s(file, "tscfix-trace%s%s-entries.bin", *tag ? "-" : "", tag);
        strcpy_s(slash + 1, MAX_PATH - (slash + 1 - path), file);
        if (!fopen_s(&f, path, "wb") && f) fwrite(g_entries, sizeof(Entry), g_entryCount, f), fclose(f);
        sprintf_s(file, "tscfix-trace%s%s-fields.bin", *tag ? "-" : "", tag);
        strcpy_s(slash + 1, MAX_PATH - (slash + 1 - path), file);
        if (!fopen_s(&f, path, "wb") && f) fwrite(g_fields, sizeof(Field), g_fieldCount, f), fclose(f);
    }
    sprintf_s(file, "tscfix-trace%s%s-modules.txt", *tag ? "-" : "", tag);
    strcpy_s(slash + 1, MAX_PATH - (slash + 1 - path), file);
    if (!fopen_s(&f, path, "w") && f) {
        HMODULE mods[1024];
        DWORD needed = 0;
        K32EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &needed);
        for (DWORD i = 0; i < needed / sizeof(HMODULE); ++i) {
            MODULEINFO mi;
            char name[MAX_PATH];
            K32GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof mi);
            GetModuleFileNameA(mods[i], name, MAX_PATH);
            fprintf(f, "%p %lx %s\n", mi.lpBaseOfDll, mi.SizeOfImage, name);
        }
        fprintf(f, "# stack %p-%p steps %llu records %zu\n", reinterpret_cast<void*>(g_stackLow),
                reinterpret_cast<void*>(g_stackHigh), static_cast<unsigned long long>(g_steps), g_count);
        fclose(f);
    }
}

}  // namespace

void TraceArm() { g_armed = true; }
bool TraceArmed() { return g_armed; }

namespace {
struct VoidCall { void (*fn)(void*, int); void* self; int pass; };
uintptr_t RunVoidCall(void* a, void*) {
    auto* c = static_cast<VoidCall*>(a);
    c->fn(c->self, c->pass);
    return 0;
}
}  // namespace

void TraceCall(void (*fn)(void*, int), void* self, int pass) {
    if (!g_armed) {
        fn(self, pass);
        return;
    }
    VoidCall c{fn, self, pass};
    TraceCall2(&RunVoidCall, &c, nullptr, "");
}

uintptr_t TraceCall2(uintptr_t (*fn)(void*, void*), void* a, void* b, const char* tag) {
    g_armed = false;
    if (!g_buf) g_buf = static_cast<Record*>(VirtualAlloc(nullptr, kMaxRecords * sizeof(Record), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!g_buf) return fn(a, b);
    MODULEINFO mi;
    HMODULE self_mod = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&TraceCall), &self_mod);
    K32GetModuleInformation(GetCurrentProcess(), self_mod, &mi, sizeof mi);
    g_selfBase = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
    g_selfEnd = g_selfBase + mi.SizeOfImage;
    auto* tib = reinterpret_cast<NT_TIB*>(NtCurrentTeb());
    g_stackLow = reinterpret_cast<uintptr_t>(tib->StackLimit);
    g_stackHigh = reinterpret_cast<uintptr_t>(tib->StackBase);
    g_tid = GetCurrentThreadId();
    g_count = 0;
    g_steps = 0;
    g_pendingAfter = nullptr;
    if (!g_starts)
        if (HMODULE engine = GetModuleHandleA("GameManagerVC64.dll")) BuildStarts(engine);
    if (g_starts && !g_entries) {
        g_entries = static_cast<Entry*>(VirtualAlloc(nullptr, kMaxEntries * sizeof(Entry), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        g_fields = static_cast<Field*>(VirtualAlloc(nullptr, kMaxFields * sizeof(Field), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    }
    g_entryCount = g_fieldCount = 0;
    g_depth = 0;
    if (!g_veh) g_veh = AddVectoredExceptionHandler(1, &StepHandler);
    g_startRsp = 0;
    LARGE_INTEGER t0, t1, freq;
    QueryPerformanceCounter(&t0);
    g_phase = 1;
    __writeeflags(__readeflags() | 0x100);  // trap flag: step from here on
    uintptr_t r = fn(a, b);
    g_phase = 3;
    __writeeflags(__readeflags() & ~0x100ull);  // in case stepping never reached the end condition
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&freq);
    Log("research: traced '%s': %llu instructions, %zu non-stack memory accesses, %zu function entries, %zu object "
        "field accesses in %.1f s", tag, static_cast<unsigned long long>(g_steps), g_count, g_entryCount, g_fieldCount,
        double(t1.QuadPart - t0.QuadPart) / freq.QuadPart);
    WriteOutput(tag);
    return r;
}

}  // namespace tscfix::tracer
