#include "VanGoghScanout.h"

namespace VanGoghScanout {

Status buildLinearPlane(const Baseline &b, const Request &r, PlaneImage *out) {
    if (out == nullptr) return Status::BadArgument;
    // 0xFFFFFFFF is what a dead/unpowered register read returns.
    if (b.surfaceConfig == UINT32_MAX || b.viewportDimension == UINT32_MAX)
        return Status::BaselineUnreadable;
    // Only reprogram a pipe that is already scanning ARGB8888; every other
    // field (rotation, mirror, alpha, DIM_TYPE) is kept from the live state.
    if ((b.surfaceConfig & kPixelFormatMask) != kPixelFormatARGB8888)
        return Status::FormatNotARGB8888;

    const uint32_t vpW = b.viewportDimension & kPitchMask;
    const uint32_t vpH = (b.viewportDimension >> 16) & kPitchMask;
    if (vpW == 0 || vpH == 0) return Status::ViewportEmpty;
    if (vpW > r.widthPixels || vpH > r.heightPixels) return Status::ViewportLargerThanSurface;

    const uint64_t minPitch = static_cast<uint64_t>(r.widthPixels) * 4u;
    if (r.pitchBytes == 0 || (r.pitchBytes % kPitchAlignBytes) != 0 || r.pitchBytes < minPitch)
        return Status::PitchInvalid;
    const uint32_t pitchPixels = r.pitchBytes / 4u;
    if (pitchPixels > kPitchMask + 1u) return Status::PitchInvalid;

    if (r.gpuAddress == 0 || (r.gpuAddress & 0xFFu) != 0 || r.gpuAddress >= kMaxGpuAddress)
        return Status::AddressInvalid;

    PlaneImage img;
    img.surfaceConfig = b.surfaceConfig;
    img.tilingConfig = b.tilingConfig & ~(kSwModeMask | kMetaLinearMask | kPipeAlignedMask);
    img.surfacePitch = pitchPixels - 1u;                       // META_PITCH cleared
    img.surfaceControl = b.surfaceControl & ~(kSurfaceControlDccMask | kSurfaceControlTmzMask);
    img.addressLow = static_cast<uint32_t>(r.gpuAddress & 0xFFFFFFFFu);
    img.addressHigh = static_cast<uint32_t>(r.gpuAddress >> 32);
    *out = img;
    return Status::Ok;
}

LiveStatus describeLiveSurface(uint32_t surfaceConfig, uint32_t tilingConfig,
                               uint32_t viewportDimension, uint32_t surfacePitch,
                               uint32_t surfaceControl, uint32_t addressLow,
                               uint32_t addressHigh, LiveSurface *out) {
    if (out == nullptr) return LiveStatus::Unreadable;
    if (surfaceConfig == UINT32_MAX || tilingConfig == UINT32_MAX ||
        viewportDimension == UINT32_MAX || surfacePitch == UINT32_MAX ||
        surfaceControl == UINT32_MAX || addressLow == UINT32_MAX ||
        addressHigh == UINT32_MAX)
        return LiveStatus::Unreadable;

    if ((surfaceConfig & kPixelFormatMask) != kPixelFormatARGB8888)
        return LiveStatus::FormatNotARGB8888;
    // DCC and any non-linear swizzle make the memory layout opaque to a plain
    // CPU mapping. Firmware GOP surfaces are linear; Linux-composited ones are not.
    if ((tilingConfig & kSwModeMask) != 0 || (surfaceControl & 0x2u) != 0)
        return LiveStatus::NotLinearUncompressed;

    const uint32_t w = viewportDimension & kPitchMask;
    const uint32_t h = (viewportDimension >> 16) & kPitchMask;
    if (w == 0 || h == 0 || w > kMaxFramebufferDimension || h > kMaxFramebufferDimension)
        return LiveStatus::ViewportInvalid;

    const uint64_t pitchBytes = static_cast<uint64_t>(surfacePitch & kPitchMask) + 1ull;
    const uint64_t pitchByteCount = pitchBytes * 4ull;
    if (pitchByteCount < static_cast<uint64_t>(w) * 4ull ||
        pitchByteCount > kMaxFramebufferPitchBytes)
        return LiveStatus::PitchInvalid;

    const uint64_t address = (static_cast<uint64_t>(addressHigh) << 32) | addressLow;
    if (address == 0 || (address & 0xFFull) != 0 || address >= kMaxGpuAddress)
        return LiveStatus::AddressInvalid;

    out->widthPixels = w;
    out->heightPixels = h;
    out->pitchBytes = static_cast<uint32_t>(pitchByteCount);
    out->rotation = (surfaceConfig >> 8) & 3u;
    out->gpuAddress = address;
    return LiveStatus::Ok;
}

bool isRotated270(const LiveSurface &surface) {
    return surface.rotation == 3;
}

const char *liveStatusName(LiveStatus status) {
    switch (status) {
        case LiveStatus::Ok: return "ok";
        case LiveStatus::Unreadable: return "HUBP surface registers unreadable";
        case LiveStatus::FormatNotARGB8888: return "live surface is not ARGB8888";
        case LiveStatus::NotLinearUncompressed: return "live surface is tiled or DCC-compressed";
        case LiveStatus::ViewportInvalid: return "live viewport is empty or too large";
        case LiveStatus::PitchInvalid: return "live pitch is invalid";
        case LiveStatus::AddressInvalid: return "live surface address is invalid";
    }
    return "unknown";
}

Geometry chooseGeometry(uint32_t surfaceConfig, uint32_t viewportDimension) {
    Geometry g{};
    if (surfaceConfig == UINT32_MAX || viewportDimension == UINT32_MAX) return g;
    if ((surfaceConfig & kPixelFormatMask) != kPixelFormatARGB8888) return g;
    const uint32_t rotation = (surfaceConfig >> 8) & 3u;
    const uint32_t w = viewportDimension & kPitchMask;
    const uint32_t h = (viewportDimension >> 16) & kPitchMask;
    if (w == 0 || h == 0) return g;
    if (rotation == 3 && w <= kLandscapeWidth && h <= kLandscapeHeight) {
        g.ok = true; g.rotated = true;
        g.widthPixels = kLandscapeWidth; g.heightPixels = kLandscapeHeight; g.pitchBytes = kLandscapePitch;
    } else if (rotation == 0 && w <= kPortraitWidth && h <= kPortraitHeight) {
        g.ok = true; g.rotated = false;
        g.widthPixels = kPortraitWidth; g.heightPixels = kPortraitHeight; g.pitchBytes = kPortraitPitch;
    }
    return g;
}

uint32_t flipControlWithLock(uint32_t flipControl, bool lock) {
    return lock ? (flipControl | 1u) : (flipControl & ~1u);
}

uint32_t underflowStatus(uint32_t hubpControl) {
    return (hubpControl >> 28) & 0x7u;                        // HUBP_UNDERFLOW_STATUS
}

const char *statusName(Status s) {
    switch (s) {
        case Status::Ok: return "ok";
        case Status::BadArgument: return "bad argument";
        case Status::BaselineUnreadable: return "HUBP registers unreadable";
        case Status::FormatNotARGB8888: return "live surface is not ARGB8888 (pipe not scanning)";
        case Status::ViewportEmpty: return "live viewport is empty";
        case Status::ViewportLargerThanSurface: return "live viewport larger than the macOS surface";
        case Status::PitchInvalid: return "pitch invalid";
        case Status::AddressInvalid: return "GPU address invalid";
    }
    return "unknown";
}

} // namespace VanGoghScanout
