#include "richpresence.h"

#include <MinHook.h>

#include <cstring>

#include "log.h"

// Steam rich presence off the game thread.
//
// The game's drive screen calls ISteamFriends::SetRichPresence for three keys every few seconds,
// on the game thread. The call is a synchronous request to the Steam client; when the client is
// slow to answer it blocks for hundreds of milliseconds (measured 2026-11-10: frames open for
// 250-300 ms every 5 s, game thread waiting inside steamclient64, in the stock engine too).
//
// This hooks the public Steamworks interface (found through steam_api64's own accessor, nothing
// game specific): a value equal to the one last given for its key is dropped, a changed one is
// handed to a worker thread that makes the call. The game always gets "true" back, which is what
// the call returns for a valid key and value.
namespace tscfix::richpresence {
namespace {

using SetFn = bool (*)(void* self, const char* key, const char* value);
constexpr int kSetRichPresenceSlot = 43;  // ISteamFriends (SteamFriends017)

struct Entry {
    char key[64];
    char value[256];
    bool dirty;
};
Entry g_entries[16];
int g_entryCount = 0;
SRWLOCK g_lock = SRWLOCK_INIT;
HANDLE g_wake = nullptr;
SetFn g_original = nullptr;
void* g_self = nullptr;
volatile LONG g_calls = 0, g_dropped = 0, g_sent = 0, g_slowMs = 0;

bool Set(void* self, const char* key, const char* value) {
    InterlockedIncrement(&g_calls);
    if (!key || strlen(key) >= sizeof g_entries[0].key || (value && strlen(value) >= sizeof g_entries[0].value))
        return g_original(self, key, value);  // unusual sizes: as before
    const char* v = value ? value : "";
    bool wake = false;
    AcquireSRWLockExclusive(&g_lock);
    g_self = self;
    Entry* e = nullptr;
    for (int i = 0; i < g_entryCount && !e; ++i)
        if (!strcmp(g_entries[i].key, key)) e = &g_entries[i];
    if (!e && g_entryCount < 16) {
        e = &g_entries[g_entryCount++];
        strcpy_s(e->key, key);
        e->value[0] = 1;  // (differs from every real value: the first one is sent)
        e->value[1] = 0;
    }
    if (e && strcmp(e->value, v)) {
        strcpy_s(e->value, v);
        e->dirty = true;
        wake = true;
    } else if (e) {
        InterlockedIncrement(&g_dropped);
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (!e) return g_original(self, key, value);  // table full: as before
    if (wake) SetEvent(g_wake);
    return true;
}

DWORD WINAPI Sender(void*) {
    for (;;) {
        WaitForSingleObject(g_wake, INFINITE);
        for (;;) {
            Entry copy{};
            void* self = nullptr;
            AcquireSRWLockExclusive(&g_lock);
            for (int i = 0; i < g_entryCount; ++i)
                if (g_entries[i].dirty) {
                    g_entries[i].dirty = false;
                    copy = g_entries[i];
                    self = g_self;
                    break;
                }
            ReleaseSRWLockExclusive(&g_lock);
            if (!self) break;
            ULONGLONG t0 = GetTickCount64();
            g_original(self, copy.key, copy.value);
            LONG ms = static_cast<LONG>(GetTickCount64() - t0);
            LONG sent = InterlockedIncrement(&g_sent);
            if (ms > g_slowMs) InterlockedExchange(&g_slowMs, ms);
            if (ms >= 100 || sent % 200 == 0)
                Log("richpresence: %ld calls by the game, %ld unchanged (dropped), %ld sent off the game thread; this one took "
                    "%ld ms, slowest %ld ms", g_calls, g_dropped, sent, ms, g_slowMs);
        }
    }
}

// Waits for the Steam API to come up, then hooks the interface method once.
DWORD WINAPI Installer(void*) {
    for (int tries = 0; tries < 600; ++tries) {
        Sleep(1000);
        HMODULE api = GetModuleHandleA("steam_api64.dll");
        if (!api) continue;
        auto accessor = reinterpret_cast<void* (*)()>(GetProcAddress(api, "SteamAPI_SteamFriends_v017"));
        if (!accessor) {
            Log("richpresence: this Steam library has no SteamFriends017 accessor; not installed");
            return 0;
        }
        void* friends = accessor();
        if (!friends) continue;  // Steam API not initialised yet
        void** vtable = *static_cast<void***>(friends);
        void* target = vtable[kSetRichPresenceSlot];
        g_wake = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        MH_Initialize();
        if (MH_CreateHook(target, reinterpret_cast<void*>(&Set), reinterpret_cast<void**>(&g_original)) != MH_OK ||
            MH_EnableHook(target) != MH_OK) {
            Log("richpresence: hook failed; not installed");
            return 0;
        }
        CloseHandle(CreateThread(nullptr, 0, &Sender, nullptr, 0, nullptr));
        Log("richpresence: Steam rich presence updates leave the game thread (unchanged values dropped)");
        return 0;
    }
    return 0;
}

}  // namespace

void Start() { CloseHandle(CreateThread(nullptr, 0, &Installer, nullptr, 0, nullptr)); }

}  // namespace tscfix::richpresence
