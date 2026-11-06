#!/usr/bin/env python3
"""Cross-check the DCN301 HUBP register table in Kext/VanGoghDCN301.cpp
against Linux's generated dcn_3_0_1_offset.h.

The kext table stores register *dword* offsets relative to the DCN base
segment (base index 2 on Van Gogh). This script resolves the same registers
from Linux's header and verifies both the dword offset and base index.

Usage:
    tools/verify_dcn301_registers.py --fetch [--ref v6.6]
    tools/verify_dcn301_registers.py --header /path/to/dcn_3_0_1_offset.h
"""

import argparse
import re
import sys
import urllib.request
from pathlib import Path

RAW_URL = (
    "https://raw.githubusercontent.com/torvalds/linux/{ref}/"
    "drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_3_0_1_offset.h"
)

# Column order must match enum VanGoghDCN301::Register.
# name -> (Linux register name template, expected base index)
REGISTERS = [
    ("SurfaceConfig", "HUBP{n}_DCSURF_SURFACE_CONFIG", 2),
    ("TilingConfig", "HUBP{n}_DCSURF_TILING_CONFIG", 2),
    ("ViewportDimension", "HUBP{n}_DCSURF_PRI_VIEWPORT_DIMENSION", 2),
    ("SurfacePitch", "HUBPREQ{n}_DCSURF_SURFACE_PITCH", 2),
    ("SurfaceAddressLow", "HUBPREQ{n}_DCSURF_PRIMARY_SURFACE_ADDRESS", 2),
    ("SurfaceAddressHigh", "HUBPREQ{n}_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH", 2),
    ("SurfaceControl", "HUBPREQ{n}_DCSURF_SURFACE_CONTROL", 2),
    ("VmidSettings", "HUBPREQ{n}_VMID_SETTINGS_0", 2),
    ("HubpControl", "HUBP{n}_DCHUBP_CNTL", 2),
    ("FlipControl", "HUBPREQ{n}_DCSURF_FLIP_CONTROL", 2),
    ("AddrConfig", "HUBP{n}_DCSURF_ADDR_CONFIG", 2),
]
HUBP_COUNT = 4


def parse_header(text):
    """Return {register name: (offset, base index)} from an offset header."""
    values = {}
    for line in text.splitlines():
        m = re.match(r"#define\s+mm([A-Z0-9_]+)\s+0x([0-9a-fA-F]+)\s*$", line)
        if not m:
            continue
        name, value = m.group(1), int(m.group(2), 16)
        if name.endswith("_BASE_IDX"):
            continue
        values[name] = value
    for line in text.splitlines():
        m = re.match(r"#define\s+mm([A-Z0-9_]+)_BASE_IDX\s+(\d+)\s*$", line)
        if not m:
            continue
        name, base = m.group(1), int(m.group(2))
        if name in values:
            values[name] = (values[name], base)
    # Registers without a base index default to the DCN segment.
    return dict(values)


def parse_kext_table(source):
    """Return [[dword, ...] for each HUBP] from VanGoghDCN301.cpp."""
    m = re.search(
        r"kRegisterDwords\[kHUBPCount\]\[kRegisterCount\]\s*=\s*\{(.*?)\}\s*;",
        source,
        re.S,
    )
    if not m:
        raise SystemExit("could not find kRegisterDwords in VanGoghDCN301.cpp")
    rows = []
    for row in re.findall(r"\{([^{}]+)\}", m.group(1)):
        values = [int(v, 16) for v in re.findall(r"0x([0-9a-fA-F]+)", row)]
        rows.append(values)
    if len(rows) != HUBP_COUNT:
        raise SystemExit(f"expected {HUBP_COUNT} HUBP rows, found {len(rows)}")
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--fetch", action="store_true", help="download the Linux header")
    ap.add_argument("--ref", default="v6.6", help="Linux ref to download (default v6.6)")
    ap.add_argument("--header", type=Path, help="use a local offset header")
    args = ap.parse_args()

    if args.fetch:
        url = RAW_URL.format(ref=args.ref)
        print(f"fetching {url}")
        with urllib.request.urlopen(url, timeout=60) as response:
            header_text = response.read().decode("utf-8")
    elif args.header:
        header_text = args.header.read_text(encoding="utf-8")
    else:
        ap.error("pass --fetch or --header")

    source = (Path(__file__).resolve().parent.parent / "Kext" / "VanGoghDCN301.cpp").read_text(
        encoding="utf-8"
    )
    table = parse_kext_table(source)
    linux = parse_header(header_text)

    failures = 0
    for hubp in range(HUBP_COUNT):
        row = table[hubp]
        if len(row) != len(REGISTERS):
            raise SystemExit(
                f"HUBP{hubp}: expected {len(REGISTERS)} columns, found {len(row)}"
            )
        for column, (label, template, expected_base) in enumerate(REGISTERS):
            name = template.format(n=hubp)
            entry = linux.get(name)
            if entry is None:
                print(f"MISSING HUBP{hubp} {label}: {name} not in header")
                failures += 1
                continue
            expected_offset, header_base = entry
            if row[column] != expected_offset:
                print(
                    f"MISMATCH HUBP{hubp} {label}: kext 0x{row[column]:04x} "
                    f"!= {name} 0x{expected_offset:04x}"
                )
                failures += 1
            if header_base != expected_base:
                print(
                    f"MISMATCH HUBP{hubp} {label}: base index {header_base} "
                    f"!= expected {expected_base}"
                )
                failures += 1

    if failures:
        print(f"verify_dcn301_registers: {failures} mismatch(es)")
        return 1
    print(
        f"verify_dcn301_registers: ok ({HUBP_COUNT * len(REGISTERS)} registers, "
        f"base index 2)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
