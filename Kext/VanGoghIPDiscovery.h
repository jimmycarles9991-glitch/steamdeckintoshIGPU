#ifndef VANGOGH_IP_DISCOVERY_H
#define VANGOGH_IP_DISCOVERY_H

#include <stddef.h>
#include <stdint.h>

namespace VanGoghIPDiscovery {

constexpr size_t kMaxEntries = 128;
constexpr size_t kMaxBases = 16;

struct Entry {
    uint16_t hwId;
    uint8_t instance;
    uint8_t major;
    uint8_t minor;
    uint8_t revision;
    uint8_t flags;
    uint8_t baseCount;
    uint32_t bases[kMaxBases];
};

struct Table {
    uint16_t binarySize;
    uint16_t ipTableVersion;
    uint16_t entryCount;
    Entry entries[kMaxEntries];
};

// Parses the AMD discovery binary used by amdgpu_discovery. No allocation and
// no hardware access; input may be a bounded copy from VRAM/TMR or firmware.
bool parse(const uint8_t *data, size_t length, Table *table);
bool find(const Table *table, uint16_t hwId, uint8_t instance, Entry *entry);

// Resolve a register-local dword offset through one discovered segment and
// ensure the complete 32-bit access fits within the supplied mapped aperture.
bool registerByteOffset(const Entry *entry, uint8_t segment,
                        uint32_t registerDword, size_t apertureLength,
                        uint32_t *byteOffset);

} // namespace VanGoghIPDiscovery

#endif
