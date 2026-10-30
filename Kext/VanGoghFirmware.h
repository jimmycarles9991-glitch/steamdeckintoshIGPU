#ifndef VANGOGH_FIRMWARE_H
#define VANGOGH_FIRMWARE_H

#include <stddef.h>
#include <stdint.h>

namespace VanGoghFirmware {

enum class ImageKind : uint8_t { ASD, TOC };

struct ImageInfo {
    uint32_t packageSize;
    uint32_t headerSize;
    uint32_t headerVersion;
    uint32_t ipVersion;
    uint32_t firmwareVersion;
    uint32_t payloadSize;
    uint32_t payloadOffset;
    uint32_t crc32;
};

// Validates the AMD common firmware container bounds and Van Gogh PSP IP
// version. This only parses bytes; it never sends an image to the device.
bool parseImage(const uint8_t *data, size_t length, ImageKind kind, ImageInfo *info);

} // namespace VanGoghFirmware

#endif
