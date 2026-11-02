#!/usr/bin/env python3
"""Read and compare the active Van Gogh DCN301 HUBP register state on Linux.

The dump command only reads the kernel's amdgpu_regs debugfs file. It never
writes registers. Capturing requires permission to read that file (normally
root); parsing and diffing saved captures do not.
"""

import argparse
import json
from pathlib import Path
import platform
import re
import struct
import sys
import tempfile
from urllib.request import Request, urlopen


SYSFS = Path("/sys/bus/pci/devices")
DEBUGFS = Path("/sys/kernel/debug/dri")
DCN301_HWID = 271
HEADER_RELATIVE = Path("drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_3_0_1_offset.h")


def collect_registers(header_text):
    """Return HUBP-indexed {register: (dword, base-index)} from AMD headers."""
    values = {}
    for match in re.finditer(r"^\s*#define\s+mm([A-Za-z0-9_]+)\s+(0x[0-9a-fA-F]+|[0-9]+)\s*$",
                             header_text, re.MULTILINE):
        name, value = match.group(1), int(match.group(2), 0)
        if name.endswith("_BASE_IDX"):
            continue
        parsed = re.match(r"HUBP(?:REQ)?(\d+)_(DCSURF_[A-Za-z0-9_]+|DCHUBP_[A-Za-z0-9_]+)$", name)
        if not parsed:
            continue
        hubp, register = int(parsed.group(1)), parsed.group(2)
        if register == "DCHUBP_CNTL_STATUS":
            continue
        base_match = re.search(r"^\s*#define\s+mm" + re.escape(name) +
                               r"_BASE_IDX\s+(0x[0-9a-fA-F]+|[0-9]+)\s*$",
                               header_text, re.MULTILINE)
        if base_match:
            values.setdefault(hubp, {})[register] = (value, int(base_match.group(1), 0))
    return values


def ip_bases(device):
    path = Path(device) / "ip_discovery/die/0" / str(DCN301_HWID) / "0/base_addr"
    try:
        return [int(value, 0) for value in path.read_text().split()]
    except (OSError, ValueError) as exc:
        raise RuntimeError(f"cannot read DCN301 base addresses from {path}: {exc}") from exc


def resolve_header(args):
    if args.header:
        return Path(args.header)
    release = platform.release()
    candidates = [
        Path("/usr/src/linux") / HEADER_RELATIVE,
        Path("/usr/src/linux-lts") / HEADER_RELATIVE,
        Path("/lib/modules") / release / "build" / HEADER_RELATIVE,
        Path("/usr/lib/modules") / release / "build" / HEADER_RELATIVE,
    ]
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    if not args.fetch:
        raise RuntimeError("DCN301 offset header not found; install matching kernel sources, "
                           "pass --header, or use --fetch")
    match = re.match(r"^(\d+\.\d+\.\d+)", release)
    if not match:
        raise RuntimeError(f"cannot derive a Linux release tag from {release!r}")
    tag = "v" + match.group(1)
    url = (f"https://raw.githubusercontent.com/torvalds/linux/{tag}/"
           "drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_3_0_1_offset.h")
    request = Request(url, headers={"User-Agent": "SteamDeckintosh-HUBP-capture"})
    try:
        with urlopen(request, timeout=20) as response:
            data = response.read()
    except OSError as exc:
        raise RuntimeError(f"could not fetch Linux {tag} DCN301 header from {url}: {exc}") from exc
    if b"mmHUBP0_DCSURF_SURFACE_CONFIG" not in data:
        raise RuntimeError(f"downloaded file from {url} is not the expected DCN301 offset header")
    path = Path(tempfile.gettempdir()) / f"dcn_3_0_1_offset-{tag}.h"
    path.write_bytes(data)
    return path


def do_dump(args):
    device = SYSFS / args.bdf
    bases = ip_bases(device)
    if len(bases) < 3:
        raise RuntimeError(f"expected DCN301 register base at index 2, found {len(bases)} bases")
    base = bases[2]
    header_path = resolve_header(args)
    registers = collect_registers(header_path.read_text())
    raw_path = Path(args.regs) if args.regs else DEBUGFS / args.bdf / "amdgpu_regs"
    raw = raw_path.read_bytes()
    result = {
        "schema": 1,
        "label": args.label,
        "bdf": args.bdf,
        "hw_id": DCN301_HWID,
        "ip_bases": bases,
        "hubp": {},
    }
    for hubp, entries in sorted(registers.items()):
        observed = {}
        for name, (dword, _base_idx) in sorted(entries.items()):
            offset = (base + dword) * 4
            if offset < 0 or offset + 4 > len(raw):
                raise RuntimeError(
                    f"{raw_path} is too short for HUBP{hubp} {name} at byte 0x{offset:x}")
            observed[name] = struct.unpack_from("<I", raw, offset)[0]
        if observed:
            result["hubp"][str(hubp)] = observed
    if not result["hubp"]:
        raise RuntimeError(f"no HUBP registers found in {header_path}")
    output = Path(args.output)
    output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    print(f"saved read-only {args.label} HUBP capture to {output}")


def rotation(config):
    return (config >> 8) & 0x3


def viewport(dimension):
    return dimension & 0xffff, (dimension >> 16) & 0xffff


def do_diff(args):
    first = json.loads(Path(args.a).read_text())
    second = json.loads(Path(args.b).read_text())
    differences = 0
    for hubp in sorted(set(first.get("hubp", {})) | set(second.get("hubp", {})),
                       key=lambda key: int(key)):
        left = first.get("hubp", {}).get(hubp, {})
        right = second.get("hubp", {}).get(hubp, {})
        for name in sorted(set(left) | set(right)):
            if name not in left or name not in right or left[name] != right[name]:
                differences += 1
                print(f"HUBP{hubp} {name}: {left.get(name, 'missing')} -> {right.get(name, 'missing')}")
                if name == "DCSURF_SURFACE_CONFIG" and name in left and name in right:
                    print(f"  ROTATION_ANGLE={rotation(left[name])} -> "
                          f"ROTATION_ANGLE={rotation(right[name])}")
                elif name == "DCSURF_PRI_VIEWPORT_DIMENSION" and name in left and name in right:
                    lw, lh = viewport(left[name]); rw, rh = viewport(right[name])
                    print(f"  WIDTH={lw} HEIGHT={lh} -> WIDTH={rw} HEIGHT={rh}")
    print(f"{differences} differing register(s) total")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    dump = commands.add_parser("dump", help="capture selected HUBP registers")
    dump.add_argument("--bdf", default="0000:04:00.0")
    dump.add_argument("--regs", help="raw amdgpu_regs file (default: debugfs)")
    dump.add_argument("--header", help="DCN301 offset header (default: matching installed kernel source)")
    dump.add_argument("--fetch", action="store_true",
                      help="fetch the header matching the running mainline Linux version")
    dump.add_argument("--label", required=True)
    dump.add_argument("-o", "--output", required=True)
    dump.set_defaults(function=do_dump)
    diff = commands.add_parser("diff", help="compare two saved captures")
    diff.add_argument("a")
    diff.add_argument("b")
    diff.set_defaults(function=do_diff)
    args = parser.parse_args(argv)
    try:
        args.function(args)
    except (OSError, ValueError, RuntimeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
