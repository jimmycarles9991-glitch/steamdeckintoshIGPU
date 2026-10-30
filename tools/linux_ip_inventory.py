#!/usr/bin/env python3
"""Print read-only PCI BAR and AMD IP discovery data from Linux sysfs."""

from pathlib import Path
import sys


SYSFS = Path("/sys/bus/pci/devices")


def read_text(path: Path) -> str:
    try:
        return path.read_text().strip()
    except OSError as exc:
        raise RuntimeError(f"cannot read {path}: {exc}") from exc


def main() -> int:
    bdf = sys.argv[1] if len(sys.argv) == 2 else "0000:04:00.0"
    if len(sys.argv) > 2:
        print(f"usage: {sys.argv[0]} [PCI-BDF]", file=sys.stderr)
        return 2

    device = SYSFS / bdf
    try:
        vendor = read_text(device / "vendor")
        device_id = read_text(device / "device")
        revision = read_text(device / "revision")
        expected_count = int(read_text(device / "ip_discovery/die/0/num_ips"), 0)

        print(f"PCI {bdf}: vendor {vendor}, device {device_id}, revision {revision}")
        print("BAR resources (OS-assigned PCI address ranges):")
        for index, line in enumerate(read_text(device / "resource").splitlines()):
            start, end, flags = (int(value, 16) for value in line.split())
            if start == 0 and end == 0:
                continue
            size = end - start + 1
            label = f"BAR{index}" if index < 6 else "Expansion ROM"
            print(f"  {label}: start=0x{start:x} size=0x{size:x} flags=0x{flags:x}")

        root = device / "ip_discovery/die/0"
        entries = []
        for hw_path in root.iterdir():
            if not hw_path.name.isdecimal() or not hw_path.is_dir():
                continue
            for instance_path in hw_path.iterdir():
                if not instance_path.name.isdecimal() or not instance_path.is_dir():
                    continue
                hw_id = int(read_text(instance_path / "hw_id"), 0)
                instance = int(read_text(instance_path / "num_instance"), 0)
                version = tuple(int(read_text(instance_path / part), 0)
                                for part in ("major", "minor", "revision"))
                bases = [int(value, 0) for value in
                         read_text(instance_path / "base_addr").split()]
                base_count = int(read_text(instance_path / "num_base_addresses"), 0)
                if len(bases) != base_count:
                    raise RuntimeError(
                        f"HWID {hw_id} instance {instance}: expected {base_count} bases, got {len(bases)}")
                entries.append((hw_id, instance, version, bases))

        entries.sort(key=lambda entry: (entry[0], entry[1]))
        if len(entries) != expected_count:
            raise RuntimeError(
                f"sysfs num_ips={expected_count}, but found {len(entries)} numeric IP entries")

        print(f"IP discovery entries: {len(entries)} (die 0)")
        print("IP bases are discovery dword offsets, not PCI addresses:")
        for hw_id, instance, version, bases in entries:
            base_text = " ".join(f"0x{base:08x}" for base in bases)
            print(f"  HWID {hw_id:3d} instance {instance}: "
                  f"{version[0]}.{version[1]}.{version[2]} bases {base_text}")
    except (OSError, ValueError, RuntimeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
