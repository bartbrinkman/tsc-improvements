#include "rtti.h"

#include <cstring>

namespace tscfix::rtti {
namespace {

#pragma pack(push, 4)
struct CompleteObjectLocator {
    DWORD signature;        // 1 on x64 (image-relative offsets)
    DWORD offset;           // offset of this vftable within the complete object
    DWORD cdOffset;
    int typeDescriptor;     // RVA
    int classDescriptor;    // RVA
    int self;               // RVA of this locator
};
struct ClassHierarchyDescriptor {
    DWORD signature;
    DWORD attributes;
    DWORD numBaseClasses;
    int baseClassArray;     // RVA of int[numBaseClasses]
};
struct BaseClassDescriptor {
    int typeDescriptor;     // RVA
    DWORD numContainedBases;
    int mdisp, pdisp, vdisp;
    DWORD attributes;
    int classDescriptor;
};
#pragma pack(pop)

struct TypeDescriptor {
    void* vftable;
    void* spare;
    char name[1];
};

bool FindSection(HMODULE module, const char* name, Section& out) {
    auto base = reinterpret_cast<BYTE*>(module);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (strncmp(reinterpret_cast<const char*>(sec->Name), name, 8) == 0) {
            out.begin = base + sec->VirtualAddress;
            out.end = out.begin + sec->Misc.VirtualSize;
            return true;
        }
    }
    return false;
}

}  // namespace

std::vector<void**> FindVTablesDerivedFrom(HMODULE module, const char* baseTypeName) {
    std::vector<void**> result;
    Section rdata;
    if (!FindSection(module, ".rdata", rdata)) return result;
    auto base = reinterpret_cast<BYTE*>(module);
    auto rva = [&](int r) { return base + r; };
    auto inImage = [&](int r) { return r > 0 && rva(r) >= rdata.begin && rva(r) < rdata.end; };
    const size_t nameLen = strlen(baseTypeName);

    std::vector<BYTE*> locators;
    for (BYTE* p = rdata.begin; p + sizeof(CompleteObjectLocator) <= rdata.end; p += 4) {
        auto col = reinterpret_cast<CompleteObjectLocator*>(p);
        if (col->signature != 1 || col->offset != 0 || rva(col->self) != p) continue;
        if (!inImage(col->classDescriptor)) continue;
        auto chd = reinterpret_cast<ClassHierarchyDescriptor*>(rva(col->classDescriptor));
        if (chd->numBaseClasses < 2 || chd->numBaseClasses > 64 || !inImage(chd->baseClassArray)) continue;
        auto bases = reinterpret_cast<int*>(rva(chd->baseClassArray));
        // Index 0 is the class itself; look for the wanted type among its bases.
        for (DWORD i = 1; i < chd->numBaseClasses; ++i) {
            if (!inImage(bases[i])) break;
            auto bcd = reinterpret_cast<BaseClassDescriptor*>(rva(bases[i]));
            auto td = reinterpret_cast<TypeDescriptor*>(rva(bcd->typeDescriptor));
            if (strncmp(td->name, baseTypeName, nameLen + 1) == 0) {
                locators.push_back(p);
                break;
            }
        }
    }

    // A vftable is preceded by a pointer to its complete object locator.
    for (BYTE* p = rdata.begin; p + 16 <= rdata.end; p += 8) {
        BYTE* target = *reinterpret_cast<BYTE**>(p);
        for (BYTE* col : locators) {
            if (target == col) result.push_back(reinterpret_cast<void**>(p + 8));
        }
    }
    return result;
}

}  // namespace tscfix::rtti
