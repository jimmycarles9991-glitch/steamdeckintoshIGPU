/* SPDX-License-Identifier: MIT */
/* One fixed mode. The GOP already set the panel timing; we only describe it. */
#include "VGAMDFramebuffer.hpp"
#include <string.h>

extern "C" {
#include "vg_modes.h"
}

IOReturn VGAMDFramebuffer::getCurrentDisplayMode(IODisplayModeID *displayMode,
                                                  IOIndex *depth) {
    if (!displayMode || !depth || !mode) return kIOReturnBadArgument;
    *displayMode = kModeID;
    *depth = 0;
    return kIOReturnSuccess;
}

IOReturn VGAMDFramebuffer::setDisplayMode(IODisplayModeID displayMode, IOIndex depth) {
    if (!mode || displayMode != kModeID || depth != 0) return kIOReturnUnsupported;
    /* Deliberately no modeset. The only mode is the one the firmware lit. */
    return kIOReturnSuccess;
}

IOReturn VGAMDFramebuffer::getStartupDisplayMode(IODisplayModeID *displayMode,
                                                  IOIndex *depth) {
    return getCurrentDisplayMode(displayMode, depth);
}

IOReturn VGAMDFramebuffer::setStartupDisplayMode(IODisplayModeID displayMode,
                                                  IOIndex depth) {
    return setDisplayMode(displayMode, depth);
}

const char *VGAMDFramebuffer::getPixelFormats(void) {
    return IO32BitDirectPixels;
}

IOItemCount VGAMDFramebuffer::getDisplayModeCount(void) {
    return mode ? 1 : 0;
}

IOReturn VGAMDFramebuffer::getDisplayModes(IODisplayModeID *allDisplayModes) {
    if (!allDisplayModes || !mode) return kIOReturnBadArgument;
    allDisplayModes[0] = kModeID;
    return kIOReturnSuccess;
}

IOReturn VGAMDFramebuffer::getInformationForDisplayMode(IODisplayModeID displayMode,
                                                         IODisplayModeInformation *info) {
    if (!info || displayMode != kModeID || !mode) return kIOReturnBadArgument;
    info->nominalWidth = mode->width;
    info->nominalHeight = mode->height;
    info->refreshRate = (IOFixed1616)vg_refresh_1616(mode);
    info->maxDepthIndex = 0;
    info->flags = 0;
    info->imageWidth = 0;
    info->imageHeight = 0;
    info->reserved[0] = info->reserved[1] = info->reserved[2] = 0;
    return kIOReturnSuccess;
}

UInt64 VGAMDFramebuffer::getPixelFormatsForDisplayMode(IODisplayModeID, IOIndex) {
    return 0;
}

IOReturn VGAMDFramebuffer::getPixelInformation(IODisplayModeID displayMode, IOIndex depth,
                                                IOPixelAperture aperture,
                                                IOPixelInformation *info) {
    if (!info || displayMode != kModeID || depth != 0 ||
        aperture != kIOFBSystemAperture || !mode)
        return kIOReturnBadArgument;
    info->bytesPerRow = mode->stride;
    info->bytesPerPlane = mode->aperture_bytes;
    info->bitsPerPixel = 32;
    info->pixelType = kIORGBDirectPixels;
    info->componentCount = 3;
    info->bitsPerComponent = 8;
    for (int i = 0; i < 8 * 2; ++i) info->componentMasks[i] = 0;
    info->componentMasks[0] = 0x00ff0000;  /* R */
    info->componentMasks[1] = 0x0000ff00;  /* G */
    info->componentMasks[2] = 0x000000ff;  /* B */
    strlcpy(info->pixelFormat, IO32BitDirectPixels, sizeof(info->pixelFormat));
    info->flags = 0;
    info->activeWidth = mode->width;
    info->activeHeight = mode->height;
    info->reserved[0] = info->reserved[1] = 0;
    return kIOReturnSuccess;
}
