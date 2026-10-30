#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>

class SteamDeckintoshProbe : public IOService {
    OSDeclareDefaultStructors(SteamDeckintoshProbe)

public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
};

OSDefineMetaClassAndStructors(SteamDeckintoshProbe, IOService)

bool SteamDeckintoshProbe::start(IOService *provider) {
    if (!IOService::start(provider)) {
        return false;
    }

    IOPCIDevice *pciDevice = OSDynamicCast(IOPCIDevice, provider);
    if (pciDevice == nullptr) {
        IOLog("SteamDeckintoshProbe: matched provider is not an IOPCIDevice\n");
        IOService::stop(provider);
        return false;
    }

    const UInt16 vendor = pciDevice->configRead16(kIOPCIConfigVendorID);
    const UInt16 device = pciDevice->configRead16(kIOPCIConfigDeviceID);
    const UInt8 revision = pciDevice->configRead8(kIOPCIConfigRevisionID);

    IOLog("SteamDeckintoshProbe: attached to PCI %04x:%04x rev %02x; probe only, no GPU registers touched\n",
          vendor, device, revision);
    return true;
}

void SteamDeckintoshProbe::stop(IOService *provider) {
    IOLog("SteamDeckintoshProbe: detached\n");
    IOService::stop(provider);
}
