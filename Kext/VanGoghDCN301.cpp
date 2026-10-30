#include "VanGoghDCN301.h"

namespace VanGoghDCN301 {

bool registerDword(uint8_t hubp, Register reg, uint32_t *outDword) {
    static constexpr uint32_t kRegisterDwords[kHUBPCount][9] = {
        {0x05E5, 0x05E7, 0x05EA, 0x0607, 0x060A, 0x060B, 0x061A, 0x0609, 0x05F3},
        {0x06C1, 0x06C3, 0x06C6, 0x06E3, 0x06E6, 0x06E7, 0x06F6, 0x06E5, 0x06CF},
        {0x079D, 0x079F, 0x07A2, 0x07BF, 0x07C2, 0x07C3, 0x07D2, 0x07C1, 0x07AB},
        {0x0879, 0x087B, 0x087E, 0x089B, 0x089E, 0x089F, 0x08AE, 0x089D, 0x0887}
    };
    const uint8_t regIndex = static_cast<uint8_t>(reg);
    if (outDword == nullptr || hubp >= kHUBPCount || regIndex >= 9) return false;
    *outDword = kRegisterDwords[hubp][regIndex];
    return true;
}

bool surfaceConfigRegisterDword(uint8_t hubp, uint32_t *outDword) {
    return registerDword(hubp, Register::SurfaceConfig, outDword);
}

uint8_t rotationFromSurfaceConfig(uint32_t surfaceConfig) {
    // DCN 3.0.1 HUBP DCSURF_SURFACE_CONFIG: ROTATION_ANGLE is bits 8..9;
    // the hardware encoding is 0, 1, 2, 3 for 0, 90, 180, 270 degrees.
    return static_cast<uint8_t>((surfaceConfig >> 8) & 0x3u);
}

uint32_t pixelFormatFromSurfaceConfig(uint32_t surfaceConfig) {
    return surfaceConfig & 0x7Fu;
}

uint32_t swizzleModeFromTilingConfig(uint32_t tilingConfig) {
    return tilingConfig & 0x1Fu;
}

uint32_t pitchPixelsFromSurfacePitch(uint32_t surfacePitch) {
    return (surfacePitch & 0x3FFFu) + 1;
}

uint32_t viewportWidth(uint32_t viewportDimension) {
    return viewportDimension & 0x3FFFu;
}

uint32_t viewportHeight(uint32_t viewportDimension) {
    return (viewportDimension >> 16) & 0x3FFFu;
}

bool dccEnabled(uint32_t surfaceControl) {
    return (surfaceControl & 0x2u) != 0;
}

bool linearUncompressedLayout(const SurfaceState *state) {
    if (state == nullptr || (state->valid & 0x7Fu) != 0x7Fu) return false;
    return swizzleModeFromTilingConfig(state->tilingConfig) == 0 &&
           !dccEnabled(state->surfaceControl);
}

} // namespace VanGoghDCN301
