#include "scripttiming.h"

#include <MinHook.h>

#include <cstdint>

#include "log.h"
#include "sites.h"

namespace tscfix::scripttiming {
namespace {

// Script manager, record call duration (game version v79.0a):
// (manager, script name string, seconds, function name string). Both strings are passed by
// value; the function releases them.
constexpr sites::Fingerprint kRecord = {0x66a5e2d974b681d9ull, 16};
// Engine string handle release (the strings' destructor).
constexpr sites::Fingerprint kRelease = {0x952668aa7be5b200ull, 48};
// The overlay switch is set by the manager's virtual method 9 (`mov byte ptr [rcx+disp8], 1`
// first thing); its offset is read from that code instead of being configured.
constexpr int kEnableSlot = 9;

using RecordFn = void (*)(void* manager, void* script, float seconds, void* function);
using ReleaseFn = void (*)(void* string);
RecordFn g_record = nullptr;
ReleaseFn g_release = nullptr;
volatile bool g_active = false;
int g_flagOffset = -1;  // -1: not read yet, 0: unknown (always record)
uint64_t g_skipped = 0, g_recorded = 0;

int FlagOffset(void* manager) {
    __try {
        void** vt = *static_cast<void***>(manager);
        auto* p = static_cast<const uint8_t*>(vt[kEnableSlot]);
        if (p[0] == 0xC6 && p[1] == 0x41 && p[3] == 0x01) return p[2];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return 0;
}

void Record(void* manager, void* script, float seconds, void* function) {
    if (g_flagOffset < 0) {
        g_flagOffset = FlagOffset(manager);
        Log("scripttiming: overlay switch %s", g_flagOffset ? "found" : "not found; recording kept");
    }
    if (g_active && g_flagOffset > 0 && !static_cast<const uint8_t*>(manager)[g_flagOffset]) {
        g_release(script);
        g_release(function);
        ++g_skipped;
        return;
    }
    ++g_recorded;
    g_record(manager, script, seconds, function);
}

}  // namespace

bool Install(HMODULE engine) {
    void* record = sites::Find(engine, kRecord);
    g_release = static_cast<ReleaseFn>(sites::Find(engine, kRelease));
    if (!record || !g_release) {
        Log("scripttiming: functions not found (game version changed?); disabled");
        return false;
    }
    MH_Initialize();
    bool ok = MH_CreateHook(record, reinterpret_cast<void*>(&Record), reinterpret_cast<void**>(&g_record)) == MH_OK &&
              MH_EnableHook(record) == MH_OK;
    Log("scripttiming: %s", ok ? "installed" : "hook failed");
    return ok;
}

void SetActive(bool active) { g_active = active; }

void OnFrame() {
    if (!g_record) return;
    static unsigned frames = 0;
    if (++frames % 300 != 0) return;
    if (g_skipped || g_recorded)
        Log("scripttiming: last 300 frames: %.1f script calls/frame, recording skipped for %.1f",
            (g_skipped + g_recorded) / 300.0, g_skipped / 300.0);
    g_skipped = g_recorded = 0;
}

}  // namespace tscfix::scripttiming
