#pragma once
#include <windows.h>

#include <cstdio>
#include <cstring>

// tscfix.ini access with presets. A preset is a named bundle of settings that one switch turns on:
// while it is active, its values take the place of whatever the ini says for those keys (except
// [experiments] keys written explicitly, see ExplicitExperiment), so a single line selects a tested
// combination. Keys outside the preset still come from the ini.
//
//   [threading] render=1     update first, then the render split over threads
namespace tscfix::config {

struct Entry {
    const char* section;
    const char* key;
    const char* value;
};

inline constexpr int kMaxEntries = 64;
inline Entry g_entries[kMaxEntries];
inline int g_count = 0;

// Adds (or replaces) a preset value. Call before the settings are read, from the loader thread.
inline void Set(const char* section, const char* key, const char* value) {
    for (int i = 0; i < g_count; ++i)
        if (!_stricmp(g_entries[i].section, section) && !_stricmp(g_entries[i].key, key)) {
            g_entries[i].value = value;
            return;
        }
    if (g_count < kMaxEntries) g_entries[g_count++] = {section, key, value};
}

inline const char* Find(const char* section, const char* key) {
    for (int i = 0; i < g_count; ++i)
        if (!_stricmp(g_entries[i].section, section) && !_stricmp(g_entries[i].key, key)) return g_entries[i].value;
    return nullptr;
}

// An [experiments] key written in the ini wins over the preset (to switch one part of a preset
// off for a test); every other preset key wins over the ini.
inline bool ExplicitExperiment(const char* section, const char* key, const char* ini) {
    if (_stricmp(section, "experiments")) return false;
    char v[8];
    GetPrivateProfileStringA(section, key, "\x01", v, sizeof v, ini);
    return v[0] != '\x01';
}

inline int Int(const char* section, const char* key, int def, const char* ini) {
    if (const char* v = Find(section, key))
        if (!ExplicitExperiment(section, key, ini)) return atoi(v);
    return static_cast<int>(GetPrivateProfileIntA(section, key, def, ini));
}

inline DWORD Str(const char* section, const char* key, const char* def, char* out, DWORD size, const char* ini) {
    if (const char* v = Find(section, key); v && !ExplicitExperiment(section, key, ini)) {
        strncpy_s(out, size, v, _TRUNCATE);
        return static_cast<DWORD>(strlen(out));
    }
    return GetPrivateProfileStringA(section, key, def, out, size, ini);
}

// [threading] render=1: the game update runs first, alone, as in the stock engine; then the
// frame's render is split over threads (shadow maps on a worker beside the main view, device
// calls replayed on the render thread). While rendering, nothing changes the world, so render
// threads only have each other to stay clear of. Measured standing at Haarlem 2026-11-10:
// 26.7 ms per frame, stock 39.0.
inline void UseRenderSplitPreset() {
    Set("render", "thread", "2");
    Set("experiments", "deferred_locks", "1");
    Set("experiments", "direct_nooverwrite", "1");
    Set("experiments", "overlap", "1");
    Set("experiments", "overlap_core", "1");
    // One thread runs the shadow pass, so the engine's single depth-only modifier has one user,
    // as in the stock engine: the per-binder clones (needed when helpers shared the shadow pass)
    // are not installed. Saves about 2% of the shadow thread.
    Set("experiments", "modifier_clones", "0");
}

}  // namespace tscfix::config
