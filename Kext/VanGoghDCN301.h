#ifndef VANGOGH_DCN301_H
#define VANGOGH_DCN301_H

#include <stdint.h>

namespace VanGoghDCN301 {

constexpr uint16_t kDisplayIPHardwareId = 271;
constexpr uint8_t kDisplayIPInstance = 0;
constexpr uint8_t kRegisterBaseSegment = 2;
constexpr uint8_t kHUBPCount = 4;

enum class Register : uint8_t {
    SurfaceConfig,
    TilingConfig,
    ViewportDimension,
    SurfacePitch,
    SurfaceAddressLow,
    SurfaceAddressHigh,
    SurfaceControl,
    VmidSettings,
    HubpControl
};

struct SurfaceState {
    uint32_t valid;
    uint32_t surfaceConfig;
    uint32_t tilingConfig;
    uint32_t viewportDimension;
    uint32_t surfacePitch;
    uint32_t surfaceAddressLow;
    uint32_t surfaceAddressHigh;
    uint32_t surfaceControl;
    uint32_t vmidSettings;
    uint32_t hubpControl;
};

bool registerDword(uint8_t hubp, Register reg, uint32_t *outDword);
bool surfaceConfigRegisterDword(uint8_t hubp, uint32_t *outDword);
uint8_t rotationFromSurfaceConfig(uint32_t surfaceConfig);
uint32_t pixelFormatFromSurfaceConfig(uint32_t surfaceConfig);
uint32_t swizzleModeFromTilingConfig(uint32_t tilingConfig);
uint32_t pitchPixelsFromSurfacePitch(uint32_t surfacePitch);
uint32_t viewportWidth(uint32_t viewportDimension);
uint32_t viewportHeight(uint32_t viewportDimension);
bool dccEnabled(uint32_t surfaceControl);
bool linearUncompressedLayout(const SurfaceState *state);

} // namespace VanGoghDCN301

#endif
