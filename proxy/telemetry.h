#pragma once
#include <cstdint>

// Values the proxy publishes for external tools (tools/raildriver.py reads them next to
// the game's own RailDriver controllers). Shared memory "Local\tscfix.telemetry".
namespace tscfix {

struct Telemetry {
    uint32_t magic;               // 'TSCF'
    uint32_t version;             // 1
    uint64_t frame;               // increments every rendered frame
    float speedLimit;             // player train, current limit (m/s)
    float nextSpeedLimit;         // next limit ahead (m/s)
    float nextSpeedLimitDistance; // distance to it (m)
    uint32_t speedLimitUpdates;   // increments on every refresh of the three values above
    // From the HUD's driver's guide (player train), version 2:
    float hudNextLimit;           // next speed limit as the HUD computes it (raw value)
    float hudNextLimitDistance;   // distance to it (m)
    float hudSignalDistance;      // distance to the next signal (m)
    uint32_t hudUpdates;          // increments whenever the HUD refreshes these values
};

Telemetry* TelemetryBlock();  // created on first use; never null after success

}  // namespace tscfix
