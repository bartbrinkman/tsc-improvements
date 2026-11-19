#pragma once
#include <windows.h>
#include <vector>

// Locate virtual tables through MSVC run-time type information (x64 layout).
// This needs only the public class name of a base class, so no bytes of the
// host program are stored in this project.
namespace tscfix::rtti {

struct Section { BYTE* begin; BYTE* end; };

// Returns the vftables of every class that has `baseTypeName` (decorated, e.g.
// ".?AVExternalInterface@GFx@Scaleform@@") among its base classes, for the
// primary (offset 0) sub-object only.
std::vector<void**> FindVTablesDerivedFrom(HMODULE module, const char* baseTypeName);

}  // namespace tscfix::rtti
