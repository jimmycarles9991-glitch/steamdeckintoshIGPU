# VanGoghGPU - display bring-up for the Steam Deck LCD iGPU.
#
#   make hosttest   # freestanding C tests; works on Linux and macOS
#   make probe      # build/out/VanGoghProbe.kext   (needs Xcode CLT)
#   make fb         # build/out/VGAMDFramebuffer.kext (needs Xcode CLT)
#   make all        # hosttest + both kexts
#   make clean

PROJ_ROOT := .
MKSDK     := $(PROJ_ROOT)/third_party/MacKernelSDK
COMMON    := $(PROJ_ROOT)/src/common
BUILD_DIR := $(PROJ_ROOT)/build
OUT       := $(BUILD_DIR)/out

CC ?= cc
XCRUN ?= xcrun
SDK   := $(shell $(XCRUN) --show-sdk-path 2>/dev/null)
KCC   := $(XCRUN) clang
KCXX  := $(XCRUN) clang++

HOST_CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Werror -I$(COMMON)
COMMON_SRCS := $(COMMON)/vg_discovery.c $(COMMON)/vg_vfct.c \
               $(COMMON)/vg_modes.c $(COMMON)/vg_bootargs.c
COMMON_HDRS := $(wildcard $(COMMON)/*.h)

KEXT_CFLAGS := -arch x86_64 -mmacosx-version-min=13.0 -isysroot $(SDK) -nostdinc \
	-std=gnu11 -O2 -fno-strict-aliasing -fwrapv -mkernel -fno-builtin -fno-common \
	-fno-stack-protector -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
	-I$(MKSDK)/Headers -I$(COMMON) -Wall -Wextra -Werror
KEXT_CXXFLAGS := -arch x86_64 -mmacosx-version-min=13.0 -isysroot $(SDK) -nostdinc \
	-std=gnu++17 -O2 -fno-strict-aliasing -mkernel -fapple-kext -fno-rtti \
	-fno-exceptions -fno-builtin -fno-common -fno-stack-protector \
	-DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
	-I$(MKSDK)/Headers -I$(COMMON) -Wall -Werror

.PHONY: all hosttest probe fb clean check-regs

all: hosttest probe fb

hosttest: $(BUILD_DIR)/hosttest
	./$(BUILD_DIR)/hosttest

$(BUILD_DIR)/hosttest: tools/hosttest.c $(COMMON_SRCS) $(COMMON_HDRS)
	mkdir -p $(BUILD_DIR)
	$(CC) $(HOST_CFLAGS) tools/hosttest.c $(COMMON_SRCS) -o $@

check-regs:
	python3 tools/check_regs.py

# --- probe kext -----------------------------------------------------------
PROBE_BUNDLE := $(OUT)/VanGoghProbe.kext
PROBE_OBJS := $(BUILD_DIR)/probe/vg_discovery.o $(BUILD_DIR)/probe/kmod_info.o \
              $(BUILD_DIR)/probe/VanGoghProbe.o

$(BUILD_DIR)/probe/%.o: $(COMMON)/%.c $(COMMON_HDRS)
	mkdir -p $(BUILD_DIR)/probe
	$(KCC) $(KEXT_CFLAGS) -c $< -o $@

$(BUILD_DIR)/probe/kmod_info.o: src/probe/kmod_info.c
	mkdir -p $(BUILD_DIR)/probe
	$(KCC) $(KEXT_CFLAGS) -c $< -o $@

$(BUILD_DIR)/probe/VanGoghProbe.o: src/probe/VanGoghProbe.cpp src/probe/VanGoghProbe.hpp $(COMMON_HDRS)
	mkdir -p $(BUILD_DIR)/probe
	$(KCXX) $(KEXT_CXXFLAGS) -c $< -o $@

probe: $(PROBE_BUNDLE)/Contents/MacOS/VanGoghProbe
$(PROBE_BUNDLE)/Contents/MacOS/VanGoghProbe: $(PROBE_OBJS) src/probe/Info.plist
	mkdir -p $(PROBE_BUNDLE)/Contents/MacOS
	$(KCXX) -arch x86_64 -mmacosx-version-min=13.0 -isysroot $(SDK) -nostdlib \
		-Xlinker -kext -L$(MKSDK)/Library/x86_64 \
		-o $@ $(PROBE_OBJS) -lkmod
	cp src/probe/Info.plist $(PROBE_BUNDLE)/Contents/Info.plist
	codesign --force --sign - $(PROBE_BUNDLE) 2>/dev/null || true

# --- framebuffer kext -----------------------------------------------------
FB_BUNDLE := $(OUT)/VGAMDFramebuffer.kext
FB_OBJS := $(BUILD_DIR)/fb/kmod_info.o $(BUILD_DIR)/fb/VGAMDFramebuffer.o \
           $(BUILD_DIR)/fb/VGAMDFramebufferModes.o $(BUILD_DIR)/fb/VGAMDFramebufferVBL.o \
           $(BUILD_DIR)/fb/common_modes.o $(BUILD_DIR)/fb/common_bootargs.o

$(BUILD_DIR)/fb/%.o: src/fb/%.cpp src/fb/VGAMDFramebuffer.hpp $(COMMON_HDRS)
	mkdir -p $(BUILD_DIR)/fb
	$(KCXX) $(KEXT_CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/fb/kmod_info.o: src/fb/kmod_info.c
	mkdir -p $(BUILD_DIR)/fb
	$(KCC) $(KEXT_CFLAGS) -c $< -o $@

$(BUILD_DIR)/fb/common_modes.o: $(COMMON)/vg_modes.c $(COMMON_HDRS)
	mkdir -p $(BUILD_DIR)/fb
	$(KCC) $(KEXT_CFLAGS) -c $< -o $@

$(BUILD_DIR)/fb/common_bootargs.o: $(COMMON)/vg_bootargs.c $(COMMON_HDRS)
	mkdir -p $(BUILD_DIR)/fb
	$(KCC) $(KEXT_CFLAGS) -c $< -o $@

fb: $(FB_BUNDLE)/Contents/MacOS/VGAMDFramebuffer
$(FB_BUNDLE)/Contents/MacOS/VGAMDFramebuffer: $(FB_OBJS) src/fb/Info.plist
	mkdir -p $(FB_BUNDLE)/Contents/MacOS
	$(KCXX) -arch x86_64 -mmacosx-version-min=13.0 -isysroot $(SDK) -nostdlib \
		-Xlinker -kext -L$(MKSDK)/Library/x86_64 \
		-o $@ $(FB_OBJS) -lkmod
	cp src/fb/Info.plist $(FB_BUNDLE)/Contents/Info.plist
	codesign --force --sign - $(FB_BUNDLE) 2>/dev/null || true

clean:
	rm -rf $(BUILD_DIR)
