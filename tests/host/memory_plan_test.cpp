// Host test for Kext/VanGoghMemoryPlan.cpp. Run with `make test`.
#include "../../Kext/VanGoghMemoryPlan.h"
#include "check.h"

using namespace VanGoghMemoryPlan;

// The Linux capture of this Deck: 4 GiB VRAM at MC 0xF400000000, 256 MiB BAR0.
static Input deck() {
    Input in{};
    in.scanoutBytes = 1280ull * 4 * 800;       // 4,096,000
    in.vramBytes = 4096 * kMiB;
    in.bar0Length = 256 * kMiB;
    in.fbBase = fbBaseFromRegister(0xF400);
    in.fbTop = fbTopFromRegister(0xF4FF);
    in.requestedOffset = kAutoOffset;
    return in;
}

int main() {
    CHECK_EQ(fbBaseFromRegister(0xF400), 0xF400000000ull);
    CHECK_EQ(fbTopFromRegister(0xF4FF), 0xF4FFFFFFFFull);
    CHECK_EQ(fbBaseFromRegister(0xFF000000u | 0xF400), 0xF400000000ull); // upper bits ignored

    Output o = plan(deck());
    CHECK(o.ok);
    CHECK_EQ(o.bytes, 4128768ull);                       // 4,096,000 rounded up to 63 * 64 KiB
    CHECK_EQ(o.vramOffset, 256 * kMiB - 4128768ull);
    CHECK_EQ(o.vramOffset % kScanoutAlign, 0);
    CHECK_EQ(o.mcAddress, 0xF400000000ull + o.vramOffset);
    CHECK(o.vramOffset + o.bytes <= 256 * kMiB);

    Input in = deck();
    in.requestedOffset = 64 * kMiB;
    o = plan(in);
    CHECK(o.ok);
    CHECK_EQ(o.vramOffset, 64 * kMiB);
    CHECK_EQ(o.mcAddress, 0xF400000000ull + 64 * kMiB);

    in.requestedOffset = 64 * kMiB + 256;
    CHECK(plan(in).status == Status::OffsetMisaligned);
    in.requestedOffset = 16 * kMiB;
    CHECK(plan(in).status == Status::OffsetBelowReserve);
    in.requestedOffset = 256 * kMiB - 64 * kKiB;         // surface would cross the end of BAR0
    CHECK(plan(in).status == Status::OffsetBeyondLimit);
    in.requestedOffset = 4000 * kMiB;                    // inside VRAM but outside BAR0
    CHECK(plan(in).status == Status::OffsetBeyondLimit);

    in = deck();
    in.bar0Length = 8192 * kMiB;
    o = plan(in);
    CHECK(o.ok);
    CHECK_EQ(o.limitEnd, 4096 * kMiB - kDefaultTopReserve);
    CHECK(o.vramOffset + o.bytes <= o.limitEnd);

    in = deck();
    in.bottomReserve = 128 * kMiB;
    in.requestedOffset = 100 * kMiB;
    CHECK(plan(in).status == Status::OffsetBelowReserve);

    in = deck(); in.scanoutBytes = 0;                    CHECK(plan(in).status == Status::ZeroLength);
    in = deck(); in.vramBytes = 0;                       CHECK(plan(in).status == Status::VramSizeInvalid);
    in = deck(); in.vramBytes = 4096 * kMiB + 1;         CHECK(plan(in).status == Status::VramSizeInvalid);
    in = deck(); in.bar0Length = 1 * kMiB;               CHECK(plan(in).status == Status::Bar0TooSmall);
    in = deck(); in.fbTop = in.fbBase - 1;               CHECK(plan(in).status == Status::FbLocationInvalid);
    in = deck(); in.fbBase += 4096;                      CHECK(plan(in).status == Status::FbLocationInvalid);
    in = deck(); in.fbTop = fbTopFromRegister(0xF400);   CHECK(plan(in).status == Status::FbLocationSmallerThanVram);
    in = deck(); in.vramBytes = 64 * kMiB; in.bar0Length = 64 * kMiB;
    CHECK(plan(in).status == Status::ReserveOverflow);   // 32 MiB + 64 MiB reserves > 64 MiB
    in = deck(); in.vramBytes = 128 * kMiB; in.bar0Length = 128 * kMiB; in.scanoutBytes = 64 * kMiB;
    CHECK(plan(in).status == Status::NoRoom);            // 32 MiB of space left for 64 MiB
    in = deck(); in.scanoutBytes = UINT64_MAX;           CHECK(!plan(in).ok);
    in = deck(); in.fbBase = (1ull << 48) - 4096 * kMiB; in.fbTop = (1ull << 48) - 1;
    o = plan(in);                                        // highest legal FB placement still fits
    CHECK(o.ok);
    CHECK(o.mcAddress + o.bytes <= (1ull << 48));
    in = deck(); in.fbBase = (1ull << 48) - 128 * kMiB; in.fbTop = in.fbBase + 4096 * kMiB - 1;
    CHECK(plan(in).status == Status::FbLocationInvalid); // top beyond the 48-bit MC space
    in = deck(); in.fbBase = 0; in.fbTop = UINT64_MAX;   CHECK(plan(in).status == Status::FbLocationInvalid);

    in = deck(); in.bar0Length = 1 * kMiB;
    o = plan(in);
    CHECK(!o.ok && o.mcAddress == 0 && o.bytes == 0);
    CHECK(statusName(Status::NoRoom) != nullptr);

    FINISH("memory_plan_test");
}
