// Host test for Kext/VanGoghScanout.cpp and the DCN301 register table.
#include "../../Kext/VanGoghDCN301.h"
#include "../../Kext/VanGoghScanout.h"
#include "check.h"

using namespace VanGoghScanout;

// What the Linux capture of this Deck reports for the active panel plane:
// ARGB8888, rotation 270 (bits 9:8 = 3), DCC enabled, swizzle 0x1b, pitch
// 1280 px, viewport 1280x800 (a single-HUBP view; ODM/MPC splits use smaller).
static Baseline liveLinux() {
    Baseline b{};
    b.surfaceConfig = 0x00000308;                    // format 8, rotation 3
    b.tilingConfig = 0x0000081B | (1u << 7);         // SW_MODE 0x1b, DIM_TYPE bit, PIPE_ALIGNED
    b.viewportDimension = (800u << 16) | 1280u;
    b.surfacePitch = (1280u - 1) | (319u << 16);     // PITCH plus a DCC META_PITCH
    b.surfaceControl = 0x00000002 | (1u << 2) | (1u << 9);   // DCC_EN, IND_BLK, secondary DCC_EN
    b.flipControl = 0x00000000;
    b.hubpControl = 0x00000000;
    return b;
}
static Request macos() {
    Request r{};
    r.gpuAddress = 0xF40FC00000ull;
    r.widthPixels = 1280; r.heightPixels = 800; r.pitchBytes = 5120;
    return r;
}

int main() {
    PlaneImage img{};
    CHECK(buildLinearPlane(liveLinux(), macos(), &img) == Status::Ok);
    CHECK_EQ(img.surfaceConfig, 0x00000308);         // rotation preserved, format 8
    CHECK_EQ(img.tilingConfig & 0x1F, 0);            // linear
    CHECK_EQ(img.tilingConfig & 0x200, 0);           // META_LINEAR clear
    CHECK_EQ(img.tilingConfig & 0x800, 0);           // PIPE_ALIGNED clear
    CHECK_EQ(img.tilingConfig & (1u << 7), 1u << 7); // DIM_TYPE kept
    CHECK_EQ(img.surfacePitch, 1279);                // META_PITCH cleared
    CHECK_EQ(img.surfaceControl & kSurfaceControlDccMask, 0);
    CHECK_EQ(img.addressLow, 0x0FC00000);
    CHECK_EQ(img.addressHigh, 0x000000F4);
    CHECK_EQ((static_cast<unsigned long long>(img.addressHigh) << 32) | img.addressLow, 0xF40FC00000ull);

    Baseline b = liveLinux();
    b.surfaceConfig = 0x00000F08;                    // ALPHA_PLANE_EN, H_MIRROR_EN, rotation 3
    b.surfaceControl |= 0x00800000;                  // an unrelated bit
    CHECK(buildLinearPlane(b, macos(), &img) == Status::Ok);
    CHECK_EQ(img.surfaceConfig, 0x00000F08);
    CHECK_EQ(img.surfaceControl, 0x00800000);

    b = liveLinux(); b.surfaceControl = 0x000F1111 | 0x2;
    CHECK(buildLinearPlane(b, macos(), &img) == Status::Ok);
    CHECK_EQ(img.surfaceControl, 0);

    b = liveLinux(); b.viewportDimension = (400u << 16) | 1280u;
    CHECK(buildLinearPlane(b, macos(), &img) == Status::Ok);
    b.viewportDimension = (800u << 16) | 640u;
    CHECK(buildLinearPlane(b, macos(), &img) == Status::Ok);

    PlaneImage sentinel{}; sentinel.surfaceConfig = 0xDEADBEEF;
    img = sentinel;
    b = liveLinux(); b.surfaceConfig = UINT32_MAX;
    CHECK(buildLinearPlane(b, macos(), &img) == Status::BaselineUnreadable);
    b = liveLinux(); b.viewportDimension = UINT32_MAX;
    CHECK(buildLinearPlane(b, macos(), &img) == Status::BaselineUnreadable);
    b = liveLinux(); b.surfaceConfig = 0x00000300;
    CHECK(buildLinearPlane(b, macos(), &img) == Status::FormatNotARGB8888);
    b = liveLinux(); b.surfaceConfig = 0x00000303;
    CHECK(buildLinearPlane(b, macos(), &img) == Status::FormatNotARGB8888);
    b = liveLinux(); b.viewportDimension = 0;
    CHECK(buildLinearPlane(b, macos(), &img) == Status::ViewportEmpty);
    b = liveLinux(); b.viewportDimension = (800u << 16) | 0;
    CHECK(buildLinearPlane(b, macos(), &img) == Status::ViewportEmpty);
    b = liveLinux(); b.viewportDimension = (1280u << 16) | 800u;    // GOP-style 800x1280 portrait
    CHECK(buildLinearPlane(b, macos(), &img) == Status::ViewportLargerThanSurface);
    CHECK_EQ(img.surfaceConfig, 0xDEADBEEF);

    Request r = macos(); r.pitchBytes = 5121;
    CHECK(buildLinearPlane(liveLinux(), r, &img) == Status::PitchInvalid);
    r = macos(); r.pitchBytes = 5000;
    CHECK(buildLinearPlane(liveLinux(), r, &img) == Status::PitchInvalid);
    r = macos(); r.pitchBytes = 4864;
    CHECK(buildLinearPlane(liveLinux(), r, &img) == Status::PitchInvalid);
    r = macos(); r.pitchBytes = 0;
    CHECK(buildLinearPlane(liveLinux(), r, &img) == Status::PitchInvalid);
    r = macos(); r.widthPixels = 0x40000000u;
    CHECK(buildLinearPlane(liveLinux(), r, &img) == Status::PitchInvalid);
    r = macos(); r.pitchBytes = 0x10000 + 256;
    CHECK(buildLinearPlane(liveLinux(), r, &img) == Status::PitchInvalid);
    r = macos(); r.pitchBytes = 0x10000;
    CHECK(buildLinearPlane(liveLinux(), r, &img) == Status::Ok);
    CHECK_EQ(img.surfacePitch, 0x3FFF);

    r = macos(); r.gpuAddress = 0;
    CHECK(buildLinearPlane(liveLinux(), r, &img) == Status::AddressInvalid);
    r = macos(); r.gpuAddress = 0xF40FC00080ull;
    CHECK(buildLinearPlane(liveLinux(), r, &img) == Status::AddressInvalid);
    r = macos(); r.gpuAddress = 1ull << 48;
    CHECK(buildLinearPlane(liveLinux(), r, &img) == Status::AddressInvalid);
    r = macos(); r.gpuAddress = (1ull << 48) - 256;
    CHECK(buildLinearPlane(liveLinux(), r, &img) == Status::Ok);
    CHECK_EQ(img.addressHigh, 0xFFFF);
    CHECK(buildLinearPlane(liveLinux(), macos(), nullptr) != Status::Ok);

    // Geometry selection from the live pipe.
    Geometry g = chooseGeometry(0x00000308, (800u << 16) | 1280u);          // rotated landscape
    CHECK(g.ok && g.rotated && g.widthPixels == 1280 && g.heightPixels == 800 && g.pitchBytes == 5120);
    g = chooseGeometry(0x00000008, (1280u << 16) | 800u);                   // native portrait (GOP-style)
    CHECK(g.ok && !g.rotated && g.widthPixels == 800 && g.heightPixels == 1280 && g.pitchBytes == 3328);
    CHECK_EQ(g.pitchBytes % 256, 0);
    CHECK(g.pitchBytes >= g.widthPixels * 4);
    CHECK(chooseGeometry(0x00000008, (1280u << 16) | 400u).ok);            // ODM half of portrait
    CHECK(chooseGeometry(0x00000308, (800u << 16) | 640u).ok);             // half of rotated landscape
    CHECK(!chooseGeometry(0x00000008, (800u << 16) | 1280u).ok);           // unrotated landscape: not ours
    CHECK(!chooseGeometry(0x00000308, (1280u << 16) | 800u).ok);           // rotated but portrait-sized
    CHECK(!chooseGeometry(0x00000108, (1280u << 16) | 800u).ok);           // rotation 1
    CHECK(!chooseGeometry(0x00000003, (1280u << 16) | 800u).ok);           // RGB565
    CHECK(!chooseGeometry(UINT32_MAX, (1280u << 16) | 800u).ok);
    CHECK(!chooseGeometry(0x00000008, UINT32_MAX).ok);
    CHECK(!chooseGeometry(0x00000008, 0).ok);
    CHECK(kMaxSurfaceBytes >= kLandscapePitch * kLandscapeHeight);
    // Both chosen geometries must be accepted by the plane builder.
    Request pr{}; pr.gpuAddress = 0xF40FC00000ull; pr.widthPixels = 800; pr.heightPixels = 1280; pr.pitchBytes = 3328;
    b = liveLinux(); b.surfaceConfig = 0x00000008; b.viewportDimension = (1280u << 16) | 800u;
    CHECK(buildLinearPlane(b, pr, &img) == Status::Ok);
    CHECK_EQ(img.surfacePitch, 831);
    CHECK_EQ(img.surfaceConfig & 0x300, 0);                                 // stays unrotated

    CHECK_EQ(flipControlWithLock(0x100, true), 0x101);
    CHECK_EQ(flipControlWithLock(0x101, false), 0x100);
    CHECK_EQ(flipControlWithLock(0x1, true), 0x1);
    CHECK_EQ(underflowStatus(0x00000000), 0);
    CHECK_EQ(underflowStatus(0x10000000), 1);
    CHECK_EQ(underflowStatus(0x70000008), 7);
    CHECK_EQ(underflowStatus(0x80000000), 0);

    for (uint8_t reg = 0; reg < VanGoghDCN301::kRegisterCount; ++reg) {
        uint32_t base = 0;
        CHECK(VanGoghDCN301::registerDword(0, static_cast<VanGoghDCN301::Register>(reg), &base));
        for (uint8_t h = 1; h < VanGoghDCN301::kHUBPCount; ++h) {
            uint32_t v = 0;
            CHECK(VanGoghDCN301::registerDword(h, static_cast<VanGoghDCN301::Register>(reg), &v));
            CHECK_EQ(v, base + h * 0xDC);
        }
    }
    uint32_t v = 0;
    CHECK(VanGoghDCN301::registerDword(1, VanGoghDCN301::Register::FlipControl, &v));  CHECK_EQ(v, 0x06F7);
    CHECK(VanGoghDCN301::registerDword(3, VanGoghDCN301::Register::AddrConfig, &v));   CHECK_EQ(v, 0x087A);
    CHECK(!VanGoghDCN301::registerDword(4, VanGoghDCN301::Register::SurfaceConfig, &v));
    CHECK(!VanGoghDCN301::registerDword(0, static_cast<VanGoghDCN301::Register>(VanGoghDCN301::kRegisterCount), &v));
    CHECK(!VanGoghDCN301::registerDword(0, VanGoghDCN301::Register::SurfaceConfig, nullptr));

    // Live-surface description used for the default firmware/OpenCore mirror.
    LiveSurface s{};
    CHECK(describeLiveSurface(0x00000008, 0x00000000, (1280u << 16) | 800u, 799u,
                              0x00000000, 0x40000000u, 0x000000F4u, &s) == LiveStatus::Ok);
    CHECK(!isRotated270(s));
    CHECK_EQ(s.widthPixels, 800u);
    CHECK_EQ(s.heightPixels, 1280u);
    CHECK_EQ(s.pitchBytes, 3200u);
    CHECK_EQ(s.rotation, 0u);
    CHECK_EQ(s.gpuAddress, 0xF440000000ull);

    CHECK(describeLiveSurface(0x00000308, 0x00000000, (800u << 16) | 1280u, 1280u - 1u,
                              0x00000000, 0x00000000u, 0x000000F4u, &s) == LiveStatus::Ok);
    CHECK(isRotated270(s));
    CHECK(!isRotated270(LiveSurface{1280, 800, 5120, 1, 0xF400000000ull}));
    CHECK_EQ(s.widthPixels, 1280u);
    CHECK_EQ(s.heightPixels, 800u);
    CHECK_EQ(s.pitchBytes, 5120u);
    CHECK_EQ(s.rotation, 3u);

    // DCC/tiling is the Linux-composited live state and cannot be adopted.
    CHECK(describeLiveSurface(0x00000308, 0x0000081B, (800u << 16) | 1280u, 1280u - 1u,
                              0x00000002, 0x40000000u, 0x000000F4u, &s) == LiveStatus::NotLinearUncompressed);
    CHECK(describeLiveSurface(0x00000308, 0x00000000, (800u << 16) | 1280u, 1280u - 1u,
                              0x00000002, 0x40000000u, 0x000000F4u, &s) == LiveStatus::NotLinearUncompressed);
    CHECK(describeLiveSurface(0x00000003, 0x00000000, (1280u << 16) | 800u, 800u - 1u,
                              0x00000000, 0x40000000u, 0x000000F4u, &s) == LiveStatus::FormatNotARGB8888);
    CHECK(describeLiveSurface(UINT32_MAX, 0, (1280u << 16) | 800u, 800u - 1u,
                              0, 0x40000000u, 0, &s) == LiveStatus::Unreadable);
    CHECK(describeLiveSurface(0x00000008, 0, (1280u << 16) | 800u, 800u - 1u,
                              0, 0x40000000u, 0, nullptr) == LiveStatus::Unreadable);
    CHECK(describeLiveSurface(0x00000008, 0, 0, 800u - 1u,
                              0, 0x40000000u, 0, &s) == LiveStatus::ViewportInvalid);
    CHECK(describeLiveSurface(0x00000008, 0, ((kMaxFramebufferDimension + 1u) << 16) | 800u,
                              800u - 1u, 0, 0x40000000u, 0, &s) == LiveStatus::ViewportInvalid);
    CHECK(describeLiveSurface(0x00000008, 0, (1280u << 16) | 800u, 800u - 2u,
                              0, 0x40000000u, 0, &s) == LiveStatus::PitchInvalid);
    CHECK(describeLiveSurface(0x00000008, 0, (1280u << 16) | 800u, kMaxFramebufferPitchBytes / 4u,
                              0, 0x40000000u, 0, &s) == LiveStatus::PitchInvalid);
    CHECK(describeLiveSurface(0x00000008, 0, (1280u << 16) | 800u, 800u - 1u,
                              0, 0, 0, &s) == LiveStatus::AddressInvalid);
    CHECK(describeLiveSurface(0x00000008, 0, (1280u << 16) | 800u, 800u - 1u,
                              0, 0x80u, 0, &s) == LiveStatus::AddressInvalid);
    CHECK(describeLiveSurface(0x00000008, 0, (1280u << 16) | 800u, 800u - 1u,
                              0, 0x00000000u, 0x00010000u, &s) == LiveStatus::AddressInvalid);
    CHECK(describeLiveSurface(0x00000108, 0, (1280u << 16) | 800u, 800u - 1u,
                              0, 0x40000000u, 0x000000F4u, &s) == LiveStatus::Ok);
    CHECK_EQ(s.rotation, 1u);
    CHECK(liveStatusName(LiveStatus::NotLinearUncompressed) != nullptr);

    FINISH("scanout_test");
}
