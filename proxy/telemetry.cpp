#include "telemetry.h"

#include <windows.h>

#include "log.h"

namespace tscfix {

Telemetry* TelemetryBlock() {
    static Telemetry* block = nullptr;
    if (block) return block;
    HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Telemetry),
                                    L"Local\\tscfix.telemetry");
    if (!map) {
        Log("telemetry: CreateFileMapping failed (%lu)", GetLastError());
        return nullptr;
    }
    block = static_cast<Telemetry*>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Telemetry)));
    if (block) {
        block->magic = 0x46435354;  // 'TSCF'
        block->version = 2;
    }
    return block;
}

}  // namespace tscfix
