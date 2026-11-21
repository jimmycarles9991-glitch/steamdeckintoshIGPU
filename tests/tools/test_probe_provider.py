#!/usr/bin/env python3
"""Regression guard for VanGoghDriver::probe().

IOService::probe() returns the matched driver instance (this), not the
provider. Casting that result to IOPCIDevice silently rejects every boot and
keeps start() from ever running. Verify the source casts the provider instead
and still honors the base probe result.
"""

from pathlib import Path
import sys

src_path = Path(__file__).resolve().parents[2] / "Kext" / "SteamDeckintosh.cpp"
src = src_path.read_text()

start = src.find("IOService *VanGoghDriver::probe(")
if start < 0:
    sys.exit("probe(): missing VanGoghDriver::probe definition")
end = src.find("\n}", start)
if end < 0:
    sys.exit("probe(): could not find end of function")
body = src[start:end]

if "OSDynamicCast(IOPCIDevice, matched)" in body:
    sys.exit("probe(): must not cast IOService::probe() result to IOPCIDevice")
if "OSDynamicCast(IOPCIDevice, provider)" not in body:
    sys.exit("probe(): must cast the provider to IOPCIDevice")
if "if (matched == nullptr)" not in body:
    sys.exit("probe(): must honor base IOService::probe() null result")
if "return matched;" not in body:
    sys.exit("probe(): must return the base probe result on success")

print("test_probe_provider: ok")
