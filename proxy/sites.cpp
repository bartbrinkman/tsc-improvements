#include "sites.h"

#include <cstring>
#include <unordered_map>
#include <vector>

namespace tscfix::sites {
namespace {

uint64_t Fnv1a64(const BYTE* p, size_t n) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

}  // namespace

void* Find(HMODULE module, const Fingerprint& fp) {
    auto base = reinterpret_cast<BYTE*>(module);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    auto funcs = reinterpret_cast<const RUNTIME_FUNCTION*>(base + dir.VirtualAddress);
    size_t count = dir.Size / sizeof(RUNTIME_FUNCTION);
    DWORD imageSize = nt->OptionalHeader.SizeOfImage;

    void* found = nullptr;
    DWORD lastBegin = ~0u;
    for (size_t i = 0; i < count; ++i) {
        DWORD begin = funcs[i].BeginAddress;
        if (begin == lastBegin || begin + fp.length > imageSize) continue;
        lastBegin = begin;
        if (Fnv1a64(base + begin, fp.length) == fp.hash) {
            if (found) return nullptr;  // ambiguous: refuse rather than patch the wrong code
            found = base + begin;
        }
    }
    return found;
}

size_t ResolveAll(HMODULE module, const Site* sites, size_t count, uintptr_t* out) {
    auto base = reinterpret_cast<BYTE*>(module);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    auto funcs = reinterpret_cast<const RUNTIME_FUNCTION*>(base + dir.VirtualAddress);
    size_t n = dir.Size / sizeof(RUNTIME_FUNCTION);
    DWORD imageSize = nt->OptionalHeader.SizeOfImage;

    // The window lengths in use (few), and for each site the function it matched (or ambiguity).
    uint8_t lengths[16] = {};
    int lengthCount = 0;
    for (size_t i = 0; i < count; ++i) {
        if (sites[i].kind == 2) continue;
        bool known = false;
        for (int k = 0; k < lengthCount; ++k) known |= lengths[k] == sites[i].length;
        if (!known && lengthCount < 16) lengths[lengthCount++] = sites[i].length;
    }
    std::vector<DWORD> match(count, 0);
    std::vector<uint8_t> hits(count, 0);
    std::unordered_map<uint64_t, std::vector<size_t>> wanted;
    for (size_t i = 0; i < count; ++i)
        if (sites[i].kind != 2) wanted[sites[i].hash].push_back(i);

    DWORD lastBegin = ~0u;
    for (size_t f = 0; f < n; ++f) {
        DWORD begin = funcs[f].BeginAddress;
        if (begin == lastBegin) continue;
        lastBegin = begin;
        for (int k = 0; k < lengthCount; ++k) {
            if (begin + lengths[k] > imageSize) continue;
            auto it = wanted.find(Fnv1a64(base + begin, lengths[k]));
            if (it == wanted.end()) continue;
            for (size_t i : it->second)
                if (sites[i].length == lengths[k] && hits[i] < 2) match[i] = begin, ++hits[i];
        }
    }

    // Kind 2: one pass over .rdata for complete object locators (signature 1) whose type name and
    // sub-object offset match; the vtable starts right after the pointer to the locator.
    std::vector<uintptr_t> vtables(count, 0);
    std::vector<uint8_t> vtHits(count, 0);
    bool anyVtable = false;
    for (size_t i = 0; i < count; ++i) anyVtable |= sites[i].kind == 2;
    if (anyVtable) {
        auto* sec = IMAGE_FIRST_SECTION(nt);
        for (int s = 0; s < nt->FileHeader.NumberOfSections; ++s) {
            if (memcmp(sec[s].Name, ".rdata", 6)) continue;
            DWORD lo = sec[s].VirtualAddress, hi = lo + sec[s].Misc.VirtualSize;
            for (DWORD p = lo; p + 8 <= hi; p += 8) {
                uintptr_t col = *reinterpret_cast<const uintptr_t*>(base + p) - reinterpret_cast<uintptr_t>(base);
                if (col < lo || col + 0x18 > hi) continue;
                auto* c = reinterpret_cast<const DWORD*>(base + col);
                if (c[0] != 1 || c[3] == 0 || c[3] + 0x20 > imageSize) continue;
                const char* name = reinterpret_cast<const char*>(base + c[3] + 0x10);
                for (size_t i = 0; i < count; ++i)
                    if (sites[i].kind == 2 && sites[i].hash == c[1] && !strcmp(sites[i].rtti, name) && vtHits[i] < 2)
                        vtables[i] = p + 8, ++vtHits[i];
            }
        }
    }

    size_t resolved = 0;
    for (size_t i = 0; i < count; ++i) {
        out[i] = 0;
        if (sites[i].kind == 2) {
            if (vtHits[i] != 1) continue;
            uintptr_t f = *reinterpret_cast<const uintptr_t*>(base + vtables[i] + 8 * sites[i].offset);
            out[i] = f - reinterpret_cast<uintptr_t>(base);
            ++resolved;
            continue;
        }
        if (hits[i] != 1) continue;  // missing or ambiguous: refuse rather than patch the wrong code
        uintptr_t at = match[i] + sites[i].offset;
        if (sites[i].kind == 1) {
            int32_t disp;
            memcpy(&disp, base + at + sites[i].dispAt, 4);
            at = at + sites[i].insnLength + disp;
        }
        out[i] = at;
        ++resolved;
    }
    return resolved;
}

}  // namespace tscfix::sites
