// Host test for the PSP readiness gate. Every outcome is driven with fake
// register/sleep callbacks so the kext never has to be exercised to test its
// stall/panic policy.
#include "../../Kext/VanGoghInitGate.h"
#include "check.h"

using namespace VanGoghInitGate;

namespace {
struct Fake {
    uint32_t bootloaderValue{0};
    uint32_t ifwiValue{0};
    uint32_t bootloaderReadyAfter{0};  // 0 means immediately
    uint32_t ifwiReadyAfter{0};
    uint32_t reads{0};
    uint32_t sleeps{0};
    bool failBootloaderRead{false};
    bool failIfwiRead{false};
    uint32_t lastDword{0};
};

bool readDword(Fake &f, uint32_t dword, uint32_t *value) {
    ++f.reads;
    f.lastDword = dword;
    if (dword == kC2Pmsg35Dword) {
        if (f.failBootloaderRead) return false;
        if (f.bootloaderReadyAfter != 0 && f.reads <= f.bootloaderReadyAfter) {
            *value = 0;
        } else {
            *value = f.bootloaderValue;
        }
        return true;
    }
    if (dword == kC2Pmsg33Dword) {
        if (f.failIfwiRead) return false;
        *value = f.ifwiValue;
        return true;
    }
    return false;
}

void sleepMs(Fake &, uint32_t) { /* no wall-clock delay in tests */ }
}  // namespace

int main() {
    // Register offsets match Linux mp_11_0_offset.h + soc15_common.h.
    CHECK_EQ(kC2Pmsg33Dword, 0x16061);
    CHECK_EQ(kC2Pmsg35Dword, 0x16063);

    CHECK(bootloaderReady(0x80000000));
    CHECK(bootloaderReady(0x90000000));   // bits 16..30 are masked out
    CHECK(!bootloaderReady(0x00000000));
    CHECK(!bootloaderReady(0x80000001));  // command field must be zero
    CHECK(!bootloaderReady(0x7FFFFFFF));
    CHECK(ifwiReady(0x80000000));
    CHECK(ifwiReady(0xFFFFFFFF));
    CHECK(!ifwiReady(0x7FFFFFFF));
    CHECK(!ifwiReady(0));

    // Happy path: bootloader ready first, then IFWI.
    {
        Fake f;
        f.bootloaderValue = 0x80000000;
        f.ifwiValue = 0x80000000;
        Result r = run([&](uint32_t d, uint32_t *v) { return readDword(f, d, v); },
                       [&](uint32_t ms) { sleepMs(f, ms); });
        CHECK(r.ok());
        CHECK_EQ(r.stage, static_cast<uint8_t>(Stage::Open));
        CHECK_EQ(r.bootloaderAttempts, 1);
        CHECK_EQ(r.ifwiAttempts, 1);
        CHECK_EQ(f.reads, 2);
    }

    // Bootloader needs several polls; the gate must not read C2PMSG_33 early.
    {
        Fake f;
        f.bootloaderValue = 0x80000000;
        f.ifwiValue = 0x80000000;
        f.bootloaderReadyAfter = 4;
        Result r = run([&](uint32_t d, uint32_t *v) { return readDword(f, d, v); },
                       [&](uint32_t ms) { sleepMs(f, ms); });
        CHECK(r.ok());
        CHECK_EQ(r.bootloaderAttempts, 5);
        CHECK_EQ(r.ifwiAttempts, 1);
        CHECK_EQ(f.reads, 6);
    }

    // Bootloader never ready: bounded, no IFWI reads, no access granted.
    {
        Fake f;
        Result r = run([&](uint32_t d, uint32_t *v) { return readDword(f, d, v); },
                       [&](uint32_t ms) { sleepMs(f, ms); });
        CHECK(!r.ok());
        CHECK_EQ(r.stage, static_cast<uint8_t>(Stage::TimedOut));
        CHECK_EQ(r.bootloaderAttempts, kBootloaderMaxAttempts);
        CHECK_EQ(r.ifwiAttempts, 0);
        CHECK_EQ(f.reads, kBootloaderMaxAttempts);
    }

    // IFWI never ready: bounded after the bootloader opens.
    {
        Fake f;
        f.bootloaderValue = 0x80000000;
        Result r = run([&](uint32_t d, uint32_t *v) { return readDword(f, d, v); },
                       [&](uint32_t ms) { sleepMs(f, ms); });
        CHECK(!r.ok());
        CHECK_EQ(r.stage, static_cast<uint8_t>(Stage::TimedOut));
        CHECK_EQ(r.ifwiAttempts, kIfwiMaxAttempts);
    }

    // A failed read at either stage stops immediately; no MMIO is touched.
    {
        Fake f;
        f.failBootloaderRead = true;
        Result r = run([&](uint32_t d, uint32_t *v) { return readDword(f, d, v); },
                       [&](uint32_t ms) { sleepMs(f, ms); });
        CHECK_EQ(r.stage, static_cast<uint8_t>(Stage::ReadFailed));
        CHECK_EQ(r.bootloaderAttempts, 1);
        CHECK_EQ(r.ifwiAttempts, 0);
    }
    {
        Fake f;
        f.bootloaderValue = 0x80000000;
        f.failIfwiRead = true;
        Result r = run([&](uint32_t d, uint32_t *v) { return readDword(f, d, v); },
                       [&](uint32_t ms) { sleepMs(f, ms); });
        CHECK_EQ(r.stage, static_cast<uint8_t>(Stage::ReadFailed));
        CHECK_EQ(r.bootloaderAttempts, 1);
        CHECK_EQ(r.ifwiAttempts, 1);
    }

    CHECK_EQ(stageName(Stage::Open)[0], 'o');
    FINISH("init_gate_test");
}
