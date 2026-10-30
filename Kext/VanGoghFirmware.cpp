#include "VanGoghFirmware.h"

namespace VanGoghFirmware {
namespace {
constexpr size_t kCommonHeaderSize = 32;
constexpr uint32_t kMaximumPackageSize = 4 * 1024 * 1024;

bool rangeFits(size_t offset, size_t size, size_t limit) {
    return offset <= limit && size <= limit - offset;
}

bool read32(const uint8_t *data, size_t length, size_t offset, uint32_t *value) {
    if (value == nullptr || !rangeFits(offset, 4, length)) return false;
    *value = static_cast<uint32_t>(data[offset]) |
             (static_cast<uint32_t>(data[offset + 1]) << 8) |
             (static_cast<uint32_t>(data[offset + 2]) << 16) |
             (static_cast<uint32_t>(data[offset + 3]) << 24);
    return true;
}

bool read16(const uint8_t *data, size_t length, size_t offset, uint16_t *value) {
    if (value == nullptr || !rangeFits(offset, 2, length)) return false;
    *value = static_cast<uint16_t>(data[offset]) |
             static_cast<uint16_t>(static_cast<uint16_t>(data[offset + 1]) << 8);
    return true;
}
}

bool parseImage(const uint8_t *data, size_t length, ImageKind kind, ImageInfo *info) {
    if (info == nullptr) return false;
    *info = ImageInfo{};
    if (data == nullptr || length < kCommonHeaderSize || length > kMaximumPackageSize)
        return false;

    ImageInfo parsed{};
    uint16_t headerMajor = 0, headerMinor = 0, ipMajor = 0, ipMinor = 0;
    if (!read32(data, length, 0, &parsed.packageSize) ||
        !read32(data, length, 4, &parsed.headerSize) ||
        !read16(data, length, 8, &headerMajor) ||
        !read16(data, length, 10, &headerMinor) ||
        !read16(data, length, 12, &ipMajor) ||
        !read16(data, length, 14, &ipMinor) ||
        !read32(data, length, 16, &parsed.firmwareVersion) ||
        !read32(data, length, 20, &parsed.payloadSize) ||
        !read32(data, length, 24, &parsed.payloadOffset) ||
        !read32(data, length, 28, &parsed.crc32)) {
        return false;
    }

    if (parsed.packageSize != length || parsed.packageSize < kCommonHeaderSize ||
        parsed.headerSize < kCommonHeaderSize || parsed.headerSize > parsed.packageSize ||
        parsed.packageSize > kMaximumPackageSize || headerMajor != 1 || headerMinor != 0 ||
        ipMajor != 11 || ipMinor != 0 || parsed.firmwareVersion == 0 ||
        parsed.payloadSize == 0 || parsed.payloadOffset < parsed.headerSize ||
        !rangeFits(parsed.payloadOffset, parsed.payloadSize, parsed.packageSize)) {
        return false;
    }

    // The two PSP images have distinct, known semantic versions on Van Gogh.
    // Do not accept a swapped or unrelated payload by accident.
    if ((kind == ImageKind::ASD && parsed.firmwareVersion != 0x21000115u) ||
        (kind == ImageKind::TOC && parsed.firmwareVersion != 1u)) {
        return false;
    }

    parsed.headerVersion = (static_cast<uint32_t>(headerMajor) << 16) | headerMinor;
    parsed.ipVersion = (static_cast<uint32_t>(ipMajor) << 16) | ipMinor;
    *info = parsed;
    return true;
}

} // namespace VanGoghFirmware
