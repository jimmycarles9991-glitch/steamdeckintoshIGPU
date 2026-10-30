#include "VanGoghIPDiscovery.h"

namespace VanGoghIPDiscovery {
namespace {
constexpr uint32_t kBinarySignature = 0x28211407;
constexpr uint32_t kIPTableSignature = 0x53445049;
constexpr size_t kBinaryHeaderSize = 12 + 6 * 8;
constexpr size_t kIPHeaderFixedSize = 14 + 16 * 4 + 2;
constexpr size_t kMaxBinarySize = 1024 * 1024;

bool rangeFits(size_t offset, size_t size, size_t limit) {
    return offset <= limit && size <= limit - offset;
}

bool read16(const uint8_t *data, size_t length, size_t offset, uint16_t *value) {
    if (value == nullptr || !rangeFits(offset, 2, length)) return false;
    *value = static_cast<uint16_t>(data[offset]) |
             static_cast<uint16_t>(static_cast<uint16_t>(data[offset + 1]) << 8);
    return true;
}

bool read32(const uint8_t *data, size_t length, size_t offset, uint32_t *value) {
    if (value == nullptr || !rangeFits(offset, 4, length)) return false;
    *value = static_cast<uint32_t>(data[offset]) |
             (static_cast<uint32_t>(data[offset + 1]) << 8) |
             (static_cast<uint32_t>(data[offset + 2]) << 16) |
             (static_cast<uint32_t>(data[offset + 3]) << 24);
    return true;
}

uint16_t checksum16(const uint8_t *data, size_t offset, size_t length) {
    uint16_t sum = 0;
    for (size_t i = 0; i < length; ++i) sum = static_cast<uint16_t>(sum + data[offset + i]);
    return sum;
}
}

bool parse(const uint8_t *data, size_t length, Table *table) {
    if (table == nullptr) return false;
    *table = Table{};
    if (data == nullptr || length < kBinaryHeaderSize) return false;

    uint32_t signature = 0;
    uint16_t expectedBinaryChecksum = 0, binarySize = 0;
    if (!read32(data, length, 0, &signature) || signature != kBinarySignature ||
        !read16(data, length, 8, &expectedBinaryChecksum) ||
        !read16(data, length, 10, &binarySize) ||
        binarySize < kBinaryHeaderSize || binarySize > length ||
        binarySize > kMaxBinarySize ||
        checksum16(data, 10, binarySize - 10) != expectedBinaryChecksum) {
        return false;
    }

    constexpr size_t kIPTableDescriptor = 12;
    uint16_t ipOffset = 0, expectedIPChecksum = 0, ipSize = 0;
    if (!read16(data, binarySize, kIPTableDescriptor, &ipOffset) ||
        !read16(data, binarySize, kIPTableDescriptor + 2, &expectedIPChecksum) ||
        !read16(data, binarySize, kIPTableDescriptor + 4, &ipSize) ||
        ipOffset < kBinaryHeaderSize || ipSize < kIPHeaderFixedSize ||
        !rangeFits(ipOffset, ipSize, binarySize) ||
        checksum16(data, ipOffset, ipSize) != expectedIPChecksum) {
        return false;
    }

    uint32_t ipSignature = 0;
    uint16_t ipVersion = 0, declaredIPSize = 0, dieCount = 0;
    if (!read32(data, binarySize, ipOffset, &ipSignature) || ipSignature != kIPTableSignature ||
        !read16(data, binarySize, ipOffset + 4, &ipVersion) || ipVersion < 1 || ipVersion > 4 ||
        !read16(data, binarySize, ipOffset + 6, &declaredIPSize) || declaredIPSize != ipSize ||
        !read16(data, binarySize, ipOffset + 12, &dieCount) || dieCount == 0 || dieCount > 16 ||
        !rangeFits(ipOffset, kIPHeaderFixedSize, ipOffset + ipSize)) {
        return false;
    }

    const size_t dieInfoStart = ipOffset + 14;
    const size_t tableEnd = ipOffset + ipSize;
    const size_t firstDieData = ipOffset + kIPHeaderFixedSize;
    const bool base64 = ipVersion >= 4 && (data[dieInfoStart + 16 * 4] & 1) != 0;
    const size_t addressWidth = base64 ? 8 : 4;
    uint16_t parsedCount = 0;

    for (uint16_t dieIndex = 0; dieIndex < dieCount; ++dieIndex) {
        uint16_t dieId = 0, dieOffset = 0;
        const size_t dieInfo = dieInfoStart + static_cast<size_t>(dieIndex) * 4;
        if (!read16(data, binarySize, dieInfo, &dieId) ||
            !read16(data, binarySize, dieInfo + 2, &dieOffset) || dieId != dieIndex ||
            dieOffset < firstDieData || !rangeFits(dieOffset, 4, tableEnd)) {
            return false;
        }

        uint16_t parsedDieId = 0, ipCount = 0;
        if (!read16(data, binarySize, dieOffset, &parsedDieId) || parsedDieId != dieId ||
            !read16(data, binarySize, dieOffset + 2, &ipCount)) {
            return false;
        }

        size_t cursor = static_cast<size_t>(dieOffset) + 4;
        for (uint16_t ipIndex = 0; ipIndex < ipCount; ++ipIndex) {
            if (!rangeFits(cursor, 8, tableEnd) || parsedCount == kMaxEntries) return false;
            Entry entry{};
            if (!read16(data, binarySize, cursor, &entry.hwId)) return false;
            entry.instance = data[cursor + 2];
            entry.baseCount = data[cursor + 3];
            entry.major = data[cursor + 4];
            entry.minor = data[cursor + 5];
            entry.revision = data[cursor + 6];
            entry.flags = data[cursor + 7];
            if (entry.baseCount > kMaxBases) return false;

            const size_t encodedBaseBytes = static_cast<size_t>(entry.baseCount) * addressWidth;
            const size_t entrySize = 8 + encodedBaseBytes;
            if (!rangeFits(cursor, entrySize, tableEnd)) return false;
            for (uint8_t baseIndex = 0; baseIndex < entry.baseCount; ++baseIndex) {
                const size_t baseOffset = cursor + 8 + static_cast<size_t>(baseIndex) * addressWidth;
                uint32_t base = 0;
                if (base64) {
                    uint32_t high = 0;
                    if (!read32(data, binarySize, baseOffset, &base) ||
                        !read32(data, binarySize, baseOffset + 4, &high) || high != 0) {
                        return false;
                    }
                    base &= 0x3fffffff;
                } else if (!read32(data, binarySize, baseOffset, &base)) {
                    return false;
                }
                entry.bases[baseIndex] = base;
            }
            table->entries[parsedCount++] = entry;
            cursor += entrySize;
        }
        if (cursor > tableEnd) return false;
    }

    table->binarySize = binarySize;
    table->ipTableVersion = ipVersion;
    table->entryCount = parsedCount;
    return parsedCount != 0;
}

bool find(const Table *table, uint16_t hwId, uint8_t instance, Entry *entry) {
    if (table == nullptr || entry == nullptr || table->entryCount > kMaxEntries) return false;
    for (uint16_t i = 0; i < table->entryCount; ++i) {
        if (table->entries[i].hwId == hwId && table->entries[i].instance == instance) {
            *entry = table->entries[i];
            return true;
        }
    }
    return false;
}

bool registerByteOffset(const Entry *entry, uint8_t segment,
                        uint32_t registerDword, size_t apertureLength,
                        uint32_t *byteOffset) {
    if (entry == nullptr || byteOffset == nullptr || segment >= entry->baseCount) return false;
    const uint64_t dwordOffset = static_cast<uint64_t>(entry->bases[segment]) + registerDword;
    const uint64_t byte = dwordOffset * 4;
    if (byte > UINT32_MAX || byte > apertureLength || 4 > apertureLength - static_cast<size_t>(byte))
        return false;
    *byteOffset = static_cast<uint32_t>(byte);
    return true;
}

} // namespace VanGoghIPDiscovery
