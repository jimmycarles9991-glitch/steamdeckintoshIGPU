#!/usr/bin/env python3
"""Resolve a Linux AMD IP register to a checked BAR5 byte range; never reads MMIO."""

from pathlib import Path
import argparse
import sys


SYSFS = Path("/sys/bus/pci/devices")
U32_MAX = (1 << 32) - 1


def parse_int(value: str) -> int:
    return int(value, 0)


def read_text(path: Path) -> str:
    try:
        return path.read_text().strip()
    except OSError as exc:
        raise RuntimeError(f"cannot read {path}: {exc}") from exc


def find_entry(device: Path, hw_id: int, instance: int):
    root = device / "ip_discovery/die/0" / str(hw_id) / str(instance)
    if not root.is_dir():
        raise RuntimeError(f"no IP discovery entry for HWID {hw_id}, instance {instance}")
    actual_id = int(read_text(root / "hw_id"), 0)
    actual_instance = int(read_text(root / "num_instance"), 0)
    if (actual_id, actual_instance) != (hw_id, instance):
        raise RuntimeError("sysfs IP entry identity did not match requested HWID/instance")
    bases = [int(value, 0) for value in read_text(root / "base_addr").split()]
    declared_count = int(read_text(root / "num_base_addresses"), 0)
    if len(bases) != declared_count:
        raise RuntimeError(f"IP entry declares {declared_count} bases but exports {len(bases)}")
    return bases


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("hw_id", type=parse_int, help="numeric IP discovery hardware ID")
    parser.add_argument("instance", type=parse_int, help="IP instance number")
    parser.add_argument("base_index", type=parse_int,
                        help="register header BASE_IDX / discovery base array index")
    parser.add_argument("reg_dword", type=parse_int,
                        help="register-local dword offset from the Van Gogh register header")
    parser.add_argument("--bdf", default="0000:04:00.0", help="PCI BDF (default: Steam Deck GPU)")
    args = parser.parse_args()

    try:
        device = SYSFS / args.bdf
        if read_text(device / "vendor").lower() != "0x1002":
            raise RuntimeError(f"{args.bdf} is not an AMD PCI function")
        if not 0 <= args.hw_id <= 0xFFFF or args.instance < 0:
            raise RuntimeError("HWID or instance is outside its valid range")
        if args.base_index < 0 or args.reg_dword < 0 or args.reg_dword > U32_MAX:
            raise RuntimeError("base index or register dword offset is invalid")

        bases = find_entry(device, args.hw_id, args.instance)
        if args.base_index >= len(bases):
            raise RuntimeError(
                f"base index {args.base_index} is out of range (entry has {len(bases)} bases)")
        base = bases[args.base_index]
        dword_offset = base + args.reg_dword
        if dword_offset > U32_MAX:
            raise RuntimeError("base plus register offset overflows the Linux 32-bit register offset")

        resources = (device / "resource").read_text().splitlines()
        if len(resources) <= 5:
            raise RuntimeError("PCI BAR5 resource is unavailable")
        bar5_start, bar5_end, _flags = (int(value, 16) for value in resources[5].split())
        if bar5_start == 0 or bar5_end < bar5_start:
            raise RuntimeError("PCI BAR5 has no valid assigned range")
        bar5_size = bar5_end - bar5_start + 1

        byte_offset = dword_offset * 4
        if byte_offset + 4 > bar5_size:
            raise RuntimeError(
                f"register byte range [0x{byte_offset:x}, 0x{byte_offset + 4:x}) "
                f"is outside direct BAR5 MMIO size 0x{bar5_size:x}; Linux may use another accessor")

        version = ".".join(read_text(device / "ip_discovery/die/0" / str(args.hw_id)
                                    / str(args.instance) / part)
                           for part in ("major", "minor", "revision"))
        print(f"PCI {args.bdf}: HWID {args.hw_id}, instance {args.instance}, IP {version}")
        print(f"base[{args.base_index}] 0x{base:x} dwords + register 0x{args.reg_dword:x} dwords")
        print(f"resolved register: 0x{dword_offset:x} dwords = BAR5 + 0x{byte_offset:x} bytes")
        print(f"current BAR5 physical range: 0x{bar5_start:x}..0x{bar5_end:x}")
        print(f"calculated physical address: 0x{bar5_start + byte_offset:x} (not mapped or read)")
    except (OSError, ValueError, RuntimeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
