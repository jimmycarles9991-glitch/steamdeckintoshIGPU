// Pure VRAM scanout placement planner. No IOKit, no hardware access: it only
// turns (VRAM size, BAR0 size, MC FB window, surface size) into an offset that
// is CPU-visible through BAR0 and clear of firmware/TMR reservations.
#ifndef VANGOGH_MEMORY_PLAN_H
#define VANGOGH_MEMORY_PLAN_H

#include <stdint.h>

namespace VanGoghMemoryPlan {

constexpr uint64_t kKiB = 1024ull;
constexpr uint64_t kMiB = 1024ull * 1024ull;
constexpr uint64_t kScanoutAlign = 64 * kKiB;
constexpr uint64_t kMaxVramBytes = 4096 * kMiB;
constexpr uint64_t kMaxGpuAddress = 1ull << 48;          // HUBP address is 48 bits
// Linux puts a 10 MiB PSP TMR near the top of VRAM (0xF4FE000000 on the Deck);
// the bottom is where firmware/GOP left its framebuffer.
constexpr uint64_t kDefaultBottomReserve = 32 * kMiB;
constexpr uint64_t kDefaultTopReserve = 64 * kMiB;
constexpr uint64_t kAutoOffset = UINT64_MAX;

enum class Status : uint8_t {
    Ok,
    ZeroLength,
    VramSizeInvalid,
    Bar0TooSmall,
    FbLocationInvalid,
    FbLocationSmallerThanVram,
    ReserveOverflow,
    NoRoom,
    OffsetMisaligned,
    OffsetBelowReserve,
    OffsetBeyondLimit
};

struct Input {
    uint64_t scanoutBytes = 0;
    uint64_t vramBytes = 0;
    uint64_t bar0Length = 0;
    uint64_t fbBase = 0;               // MC address of VRAM start
    uint64_t fbTop = 0;                // MC address of last VRAM byte
    uint64_t requestedOffset = kAutoOffset;
    uint64_t bottomReserve = kDefaultBottomReserve;
    uint64_t topReserve = kDefaultTopReserve;
};

struct Output {
    bool ok = false;
    Status status = Status::ZeroLength;
    uint64_t vramOffset = 0;           // also the BAR0 offset
    uint64_t bytes = 0;                // scanoutBytes rounded up to kScanoutAlign
    uint64_t mcAddress = 0;            // what HUBP's address registers take
    uint64_t limitEnd = 0;             // exclusive end of the usable window
};

// MC_VM_FB_LOCATION_* hold bits 47:24 in a 24-bit field.
uint64_t fbBaseFromRegister(uint32_t reg);
uint64_t fbTopFromRegister(uint32_t reg);

Output plan(const Input &in);
const char *statusName(Status s);

} // namespace VanGoghMemoryPlan

#endif
