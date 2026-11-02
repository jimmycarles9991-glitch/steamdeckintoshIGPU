# Host-side tests for the pure logic in Kext/. The kext itself is built by
# Xcode (see .github/workflows/build.yml); these need only g++ and make.
CXX ?= g++
CXXFLAGS ?= -std=gnu++14 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined
BUILD := build/host
LOGIC := Kext/VanGoghMemoryPlan.cpp Kext/VanGoghScanout.cpp Kext/VanGoghDCN301.cpp

.PHONY: test verify-registers compile-check clean-host
test: $(BUILD)/memory_plan_test $(BUILD)/scanout_test
	$(BUILD)/memory_plan_test
	$(BUILD)/scanout_test
	python3 tests/tools/test_hubp_dump.py

$(BUILD)/memory_plan_test: tests/host/memory_plan_test.cpp Kext/VanGoghMemoryPlan.cpp Kext/VanGoghMemoryPlan.h tests/host/check.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) tests/host/memory_plan_test.cpp Kext/VanGoghMemoryPlan.cpp -o $@

$(BUILD)/scanout_test: tests/host/scanout_test.cpp $(LOGIC) Kext/VanGoghScanout.h Kext/VanGoghDCN301.h tests/host/check.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) tests/host/scanout_test.cpp Kext/VanGoghScanout.cpp Kext/VanGoghDCN301.cpp -o $@

# Cross-check register constants against Linux's own headers (needs network).
verify-registers:
	python3 tools/verify_dcn301_registers.py --fetch

clean-host:
	rm -rf $(BUILD)

# Compile the kext sources for macOS with a bundled clang (see the script header).
compile-check:
	tools/compile_check_kext.sh
