// Pure HUBP linear-surface register planner. Takes registers read back from the
// live pipe (Baseline) plus the surface macOS wants scanned out (Request) and
// produces the register values to write (PlaneImage), or refuses.
#ifndef VANGOGH_SCANOUT_H
#define VANGOGH_SCANOUT_H

#include <stdint.h>

namespace VanGoghScanout {

// HUBPREQ_DCSURF_SURFACE_CONTROL: DCC_EN/IND_BLK (+C, secondary) and TMZ bits
// (Linux dcn_3_0_1_sh_mask.h). A linear surface has neither.
constexpr uint32_t kSurfaceControlDccMask = 0x00006E6E;
constexpr uint32_t kSurfaceControlTmzMask = 0x000F1111;
constexpr uint32_t kSwModeMask = 0x0000001F;
constexpr uint32_t kMetaLinearMask = 0x00000200;
constexpr uint32_t kPipeAlignedMask = 0x00000800;
constexpr uint32_t kPixelFormatMask = 0x0000007F;
constexpr uint32_t kPixelFormatARGB8888 = 8;
constexpr uint32_t kPitchMask = 0x00003FFF;
constexpr uint32_t kPitchAlignBytes = 256;
constexpr uint64_t kMaxGpuAddress = 1ull << 48;

enum class Status : uint8_t {
    Ok,
    BadArgument,
    BaselineUnreadable,
    FormatNotARGB8888,
    ViewportEmpty,
    ViewportLargerThanSurface,
    PitchInvalid,
    AddressInvalid
};

struct Baseline {
    uint32_t surfaceConfig;
    uint32_t tilingConfig;
    uint32_t viewportDimension;
    uint32_t surfacePitch;
    uint32_t surfaceControl;
    uint32_t flipControl;
    uint32_t hubpControl;
};

struct Request {
    uint64_t gpuAddress;
    uint32_t widthPixels;
    uint32_t heightPixels;
    uint32_t pitchBytes;
};

struct PlaneImage {
    uint32_t surfaceConfig;
    uint32_t tilingConfig;
    uint32_t surfacePitch;
    uint32_t surfaceControl;
    uint32_t addressLow;
    uint32_t addressHigh;
};

// The two surfaces this driver knows how to scan out, chosen from what the
// firmware/OS left running on the panel HUBP.
constexpr uint32_t kLandscapeWidth = 1280, kLandscapeHeight = 800, kLandscapePitch = 5120;
// 800 px * 4 = 3200 bytes is not 256-aligned, so the portrait pitch is padded.
constexpr uint32_t kPortraitWidth = 800, kPortraitHeight = 1280, kPortraitPitch = 3328;
constexpr uint32_t kMaxSurfaceBytes = kPortraitPitch * kPortraitHeight;   // >= landscape size

struct Geometry {
    bool ok;
    uint32_t widthPixels;
    uint32_t heightPixels;
    uint32_t pitchBytes;
    bool rotated;       // true: HUBP rotates a landscape source to the portrait panel
};

// ARGB8888 + rotation 270 + viewport within 1280x800  -> landscape source, rotated.
// ARGB8888 + rotation 0   + viewport within 800x1280  -> native portrait, no rotation.
// Anything else (other format/rotation/size, unreadable) -> ok == false.
Geometry chooseGeometry(uint32_t surfaceConfig, uint32_t viewportDimension);

Status buildLinearPlane(const Baseline &baseline, const Request &request, PlaneImage *out);
uint32_t flipControlWithLock(uint32_t flipControl, bool lock);
uint32_t underflowStatus(uint32_t hubpControl);
const char *statusName(Status s);

} // namespace VanGoghScanout

#endif
