// Bounded PSP readiness gate for Van Gogh hardware bring-up.
//
// Derived from Linux v6.18.44 (the kernel running on the reference Deck):
//   drivers/gpu/drm/amd/amdgpu/amdgpu_discovery.c
//       waits for MP0_SMN_C2PMSG_33 bit 31 (IFWI init) before reading the
//       discovery TMR: 2000 x msleep(1).
//   drivers/gpu/drm/amd/amdgpu/psp_v11_0.c
//       psp_v11_0_wait_for_bootloader() waits for MP0_SMN_C2PMSG_35 bit 31
//       with mask 0x8000FFFF through psp_wait_for() (bounded by usec_timeout,
//       1 us polling). This is the gate that must open BEFORE any other GPU
//       MMIO is touched; the kext previously only waited on C2PMSG_33.
//
// MP0 base index 0 is dword 0x16000 (soc15_common.h + mp_11_0_offset.h):
//   C2PMSG_33 = 0x16000 + 0x061, C2PMSG_35 = 0x16000 + 0x063.
//
// The gate is pure policy: no IOKit, no hardware access. It takes a dword
// reader and a millisecond sleeper so host tests can drive every outcome.
#ifndef VANGOGH_INIT_GATE_H
#define VANGOGH_INIT_GATE_H

#include <stdint.h>

namespace VanGoghInitGate {

constexpr uint32_t kMp0BaseDword = 0x16000;
constexpr uint32_t kC2Pmsg33Dword = kMp0BaseDword + 0x061;  // IFWI init
constexpr uint32_t kC2Pmsg35Dword = kMp0BaseDword + 0x063;  // PSP bootloader

constexpr uint32_t kIfwiReadyMask = 0x80000000u;
constexpr uint32_t kBootloaderReadyMask = 0x8000FFFFu;
constexpr uint32_t kBootloaderReadyValue = 0x80000000u;

// Linux psp_wait_for uses a 1,000,000 us ceiling with 1 us polling. The
// deferred worker polls at 1 ms, so 1000 attempts cover the same 1 s window.
// The 20x outer retry in psp_v11_0_wait_for_bootloader is for reset/S3 paths
// which do not apply to a deferred first-boot probe.
constexpr uint32_t kPollIntervalMs = 1;
constexpr uint32_t kBootloaderMaxAttempts = 1000;
constexpr uint32_t kIfwiMaxAttempts = 2000;  // Linux discovery: 2000 x msleep(1)

enum class Stage : uint8_t {
    NotStarted = 0,
    WaitingBootloader,
    WaitingIfwi,
    Open,
    ReadFailed,
    TimedOut
};

struct Result {
    Stage stage{Stage::NotStarted};
    uint32_t bootloaderAttempts{0};
    uint32_t ifwiAttempts{0};

    bool ok() const { return stage == Stage::Open; }
};

// True when the value reported by C2PMSG_35 means the PSP bootloader is
// ready to accept host commands. Bits 15:0 are the command field and must
// read back as zero for the ready indication (psp_v11_0.c).
inline bool bootloaderReady(uint32_t value) {
    return (value & kBootloaderReadyMask) == kBootloaderReadyValue;
}

// True when C2PMSG_33 reports IFWI initialization complete (discovery.c).
inline bool ifwiReady(uint32_t value) {
    return (value & kIfwiReadyMask) != 0;
}

// Run both waits in the order Linux uses: bootloader first, then IFWI.
// ReadFn:  bool (uint32_t dword, uint32_t *value)
// SleepFn: void (uint32_t milliseconds)
// Every loop is bounded; there is no path that waits forever.
template <typename ReadFn, typename SleepFn>
Result run(ReadFn readDword, SleepFn sleepMs) {
    Result result{};
    result.stage = Stage::WaitingBootloader;
    uint32_t value = 0;

    for (uint32_t i = 0; i < kBootloaderMaxAttempts; ++i) {
        ++result.bootloaderAttempts;
        if (!readDword(kC2Pmsg35Dword, &value)) {
            result.stage = Stage::ReadFailed;
            return result;
        }
        if (bootloaderReady(value)) {
            result.stage = Stage::WaitingIfwi;
            break;
        }
        sleepMs(kPollIntervalMs);
    }
    if (result.stage != Stage::WaitingIfwi) {
        result.stage = Stage::TimedOut;
        return result;
    }

    for (uint32_t i = 0; i < kIfwiMaxAttempts; ++i) {
        ++result.ifwiAttempts;
        if (!readDword(kC2Pmsg33Dword, &value)) {
            result.stage = Stage::ReadFailed;
            return result;
        }
        if (ifwiReady(value)) {
            result.stage = Stage::Open;
            return result;
        }
        sleepMs(kPollIntervalMs);
    }
    result.stage = Stage::TimedOut;
    return result;
}

inline uint8_t stageCode(Stage stage) {
    return static_cast<uint8_t>(stage);
}

inline const char *stageName(Stage stage) {
    switch (stage) {
        case Stage::NotStarted: return "not-started";
        case Stage::WaitingBootloader: return "waiting-bootloader";
        case Stage::WaitingIfwi: return "waiting-ifwi";
        case Stage::Open: return "open";
        case Stage::ReadFailed: return "read-failed";
        case Stage::TimedOut: return "timed-out";
    }
    return "unknown";
}

}  // namespace VanGoghInitGate

#endif  // VANGOGH_INIT_GATE_H
