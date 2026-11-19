#include "fastpaths.h"

#include <MinHook.h>

#include <intrin.h>

#include <cstdint>
#include <cstring>

#include "log.h"
#include "sites.h"

namespace tscfix::fastpaths {
namespace {

// Engine string compare (game version v79.0a): (a, b, if-less, if-greater, if-equal, length of
// a, length of b) -> one of the three values; a length of -1 means "measure it". Characters
// compare as signed chars; a proper prefix is less.
constexpr sites::Fingerprint kStringCompare = {0xea0233b6c502313bull, 32};
using CompareFn = uint32_t (*)(const char* a, const char* b, uint8_t less, uint8_t greater, uint8_t equal,
                               int lengthA, int lengthB);
CompareFn g_compare = nullptr;
volatile bool g_active = false;

uint32_t StringCompare(const char* a, const char* b, uint8_t less, uint8_t greater, uint8_t equal, int lengthA,
                       int lengthB) {
    if (!g_active) return g_compare(a, b, less, greater, equal, lengthA, lengthB);
    if (lengthA == -1) lengthA = a ? static_cast<int>(strlen(a)) : 0;
    if (lengthB == -1) lengthB = b ? static_cast<int>(strlen(b)) : 0;
    if (lengthA < 0 || lengthB < 0) return g_compare(a, b, less, greater, equal, lengthA, lengthB);
    size_t n = static_cast<size_t>(lengthA < lengthB ? lengthA : lengthB);
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t x, y;
        memcpy(&x, a + i, 8);
        memcpy(&y, b + i, 8);
        if (x != y) {
            unsigned long bit;
            _BitScanForward64(&bit, x ^ y);
            i += bit / 8;
            return static_cast<signed char>(a[i]) < static_cast<signed char>(b[i]) ? less : greater;
        }
    }
    for (; i < n; ++i)
        if (a[i] != b[i]) return static_cast<signed char>(a[i]) < static_cast<signed char>(b[i]) ? less : greater;
    if (lengthA == lengthB) return equal;
    return lengthA < lengthB ? less : greater;
}

}  // namespace

bool Install(HMODULE engine) {
    void* compare = sites::Find(engine, kStringCompare);
    if (!compare) {
        Log("fastpaths: string compare not found (game version changed?); disabled");
        return false;
    }
    MH_Initialize();
    bool ok = MH_CreateHook(compare, reinterpret_cast<void*>(&StringCompare), reinterpret_cast<void**>(&g_compare)) ==
                  MH_OK &&
              MH_EnableHook(compare) == MH_OK;
    Log("fastpaths: string compare %s", ok ? "replaced" : "hook failed");
    return ok;
}

void SetActive(bool active) { g_active = active; }

}  // namespace tscfix::fastpaths
