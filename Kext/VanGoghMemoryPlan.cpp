#include "VanGoghMemoryPlan.h"

namespace VanGoghMemoryPlan {

uint64_t fbBaseFromRegister(uint32_t reg) {
    return static_cast<uint64_t>(reg & 0x00FFFFFFu) << 24;
}

uint64_t fbTopFromRegister(uint32_t reg) {
    return (static_cast<uint64_t>(reg & 0x00FFFFFFu) << 24) | 0x00FFFFFFull;
}

static Output fail(Status s) {
    Output o;
    o.ok = false;
    o.status = s;
    return o;
}

Output plan(const Input &in) {
    if (in.scanoutBytes == 0) return fail(Status::ZeroLength);
    if (in.vramBytes == 0 || in.vramBytes > kMaxVramBytes) return fail(Status::VramSizeInvalid);
    if (in.bar0Length <= in.bottomReserve) return fail(Status::Bar0TooSmall);

    // The 24-bit register fields mean a real top is always below 2^48;
    // anything else was not read from a live block.
    if (in.fbTop < in.fbBase || in.fbTop >= kMaxGpuAddress || (in.fbBase & (kMiB - 1)) != 0)
        return fail(Status::FbLocationInvalid);
    if (in.fbTop - in.fbBase + 1 < in.vramBytes) return fail(Status::FbLocationSmallerThanVram);

    if (in.bottomReserve > in.vramBytes || in.topReserve > in.vramBytes ||
        in.bottomReserve + in.topReserve > in.vramBytes)
        return fail(Status::ReserveOverflow);

    if (in.scanoutBytes > UINT64_MAX - (kScanoutAlign - 1)) return fail(Status::NoRoom);
    const uint64_t bytes = (in.scanoutBytes + kScanoutAlign - 1) & ~(kScanoutAlign - 1);

    // Window: above the bottom reserve, below the top reserve, and inside what
    // the CPU can reach through BAR0.
    uint64_t limitEnd = in.vramBytes - in.topReserve;
    if (in.bar0Length < limitEnd) limitEnd = in.bar0Length;
    const uint64_t floorOffset = (in.bottomReserve + kScanoutAlign - 1) & ~(kScanoutAlign - 1);

    uint64_t offset;
    if (in.requestedOffset == kAutoOffset) {
        if (bytes > limitEnd || limitEnd - bytes < floorOffset) return fail(Status::NoRoom);
        offset = (limitEnd - bytes) & ~(kScanoutAlign - 1);
        if (offset < floorOffset) return fail(Status::NoRoom);
    } else {
        offset = in.requestedOffset;
        if ((offset & (kScanoutAlign - 1)) != 0) return fail(Status::OffsetMisaligned);
        if (offset < in.bottomReserve) return fail(Status::OffsetBelowReserve);
        if (offset > limitEnd || bytes > limitEnd - offset) return fail(Status::OffsetBeyondLimit);
    }

    // fbBase + vramBytes <= fbTop + 1 <= 2^48 and offset + bytes <= vramBytes,
    // so the sum cannot overflow or leave the 48-bit HUBP address range.
    Output o;
    o.ok = true;
    o.status = Status::Ok;
    o.vramOffset = offset;
    o.bytes = bytes;
    o.mcAddress = in.fbBase + offset;
    o.limitEnd = limitEnd;
    return o;
}

const char *statusName(Status s) {
    switch (s) {
        case Status::Ok: return "ok";
        case Status::ZeroLength: return "zero-length scanout";
        case Status::VramSizeInvalid: return "VRAM size invalid";
        case Status::Bar0TooSmall: return "BAR0 does not cover the reserved region";
        case Status::FbLocationInvalid: return "MC FB location invalid";
        case Status::FbLocationSmallerThanVram: return "MC FB window smaller than VRAM";
        case Status::ReserveOverflow: return "reserves exceed VRAM";
        case Status::NoRoom: return "no room for scanout in the CPU-visible window";
        case Status::OffsetMisaligned: return "requested offset not 64 KiB aligned";
        case Status::OffsetBelowReserve: return "requested offset inside bottom reserve";
        case Status::OffsetBeyondLimit: return "requested surface passes the usable window";
    }
    return "unknown";
}

} // namespace VanGoghMemoryPlan
