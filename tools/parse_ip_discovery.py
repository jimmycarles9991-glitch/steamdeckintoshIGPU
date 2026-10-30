#!/usr/bin/env python3
"""Bounds-check and print an AMD IP discovery binary captured from Linux."""

from pathlib import Path
import argparse
import struct
import sys


BINARY_SIGNATURE = 0x28211407
IP_TABLE_SIGNATURE = 0x53445049
IP_DISCOVERY_TABLE = 0
HEADER_SIZE = 12 + 6 * 8
MAX_TABLE_BYTES = 1 << 20


class ParseError(ValueError):
    pass


def require_range(data: bytes, offset: int, size: int, limit: int, what: str) -> None:
    if offset < 0 or size < 0 or offset > limit or size > limit - offset:
        raise ParseError(f"{what} range 0x{offset:x}+0x{size:x} exceeds 0x{limit:x}")


def u16(data: bytes, offset: int, limit: int, what: str) -> int:
    require_range(data, offset, 2, limit, what)
    return struct.unpack_from("<H", data, offset)[0]


def u32(data: bytes, offset: int, limit: int, what: str) -> int:
    require_range(data, offset, 4, limit, what)
    return struct.unpack_from("<I", data, offset)[0]


def additive_checksum(data: bytes) -> int:
    return sum(data) & 0xFFFF


def parse_blob(data: bytes):
    if len(data) < HEADER_SIZE:
        raise ParseError("binary is shorter than the AMD discovery header")
    if u32(data, 0, len(data), "binary signature") != BINARY_SIGNATURE:
        raise ParseError("invalid AMD discovery binary signature")

    expected_binary_checksum = u16(data, 8, len(data), "binary checksum")
    binary_size = u16(data, 10, len(data), "binary size")
    if binary_size < HEADER_SIZE or binary_size > len(data):
        raise ParseError(f"binary size 0x{binary_size:x} is outside the supplied blob")
    if binary_size > MAX_TABLE_BYTES:
        raise ParseError(f"binary size 0x{binary_size:x} exceeds parser limit")
    if additive_checksum(data[10:binary_size]) != expected_binary_checksum:
        raise ParseError("binary checksum mismatch")

    table_offset = 12 + IP_DISCOVERY_TABLE * 8
    ip_offset = u16(data, table_offset, binary_size, "IP table offset")
    expected_ip_checksum = u16(data, table_offset + 2, binary_size, "IP table checksum")
    table_size = u16(data, table_offset + 4, binary_size, "IP table size")
    if table_offset + 8 > binary_size:
        raise ParseError("IP table descriptor is truncated")
    if not ip_offset or not table_size:
        raise ParseError("binary has no IP discovery table")
    if ip_offset < HEADER_SIZE:
        raise ParseError("IP discovery table overlaps the binary header")
    require_range(data, ip_offset, table_size, binary_size, "IP discovery table")
    if additive_checksum(data[ip_offset:ip_offset + table_size]) != expected_ip_checksum:
        raise ParseError("IP discovery table checksum mismatch")

    if u32(data, ip_offset, binary_size, "IP table signature") != IP_TABLE_SIGNATURE:
        raise ParseError("invalid IP discovery table signature")
    table_version = u16(data, ip_offset + 4, binary_size, "IP table version")
    header_table_size = u16(data, ip_offset + 6, binary_size, "IP table header size")
    if table_version < 1 or table_version > 4:
        raise ParseError(f"unsupported IP discovery table version {table_version}")
    if header_table_size != table_size:
        raise ParseError(
            f"IP table descriptor size {table_size} disagrees with header size {header_table_size}")
    num_dies = u16(data, ip_offset + 12, binary_size, "IP table die count")
    if num_dies > 16:
        raise ParseError(f"IP discovery die count {num_dies} exceeds format limit")

    # ip_discovery_header has a fixed 16-entry die_info array. Version 4 adds
    # the base_addr_64_bit flag in the two bytes after that array.
    table_end = ip_offset + table_size
    die_info_start = ip_offset + 14
    require_range(data, die_info_start, 16 * 4, table_end, "die info array")
    require_range(data, die_info_start + 16 * 4, 2, table_end, "IP header padding/base flag")
    is_64bit = table_version >= 4 and bool(data[die_info_start + 16 * 4] & 1)
    address_width = 8 if is_64bit else 4
    first_die_data = die_info_start + 16 * 4 + 2
    entries = []

    for die_index in range(num_dies):
        info = die_info_start + die_index * 4
        die_id = u16(data, info, binary_size, "die ID")
        die_offset = u16(data, info + 2, binary_size, "die offset")
        if die_id != die_index:
            raise ParseError(f"die ID {die_id} is out of order; expected {die_index}")
        if die_offset < first_die_data:
            raise ParseError(f"die {die_id} header overlaps the IP discovery header")
        require_range(data, die_offset, 4, table_end, f"die {die_id} header")
        parsed_die_id = u16(data, die_offset, binary_size, "die header ID")
        num_ips = u16(data, die_offset + 2, binary_size, "die IP count")
        if parsed_die_id != die_id:
            raise ParseError(f"die header ID {parsed_die_id} does not match directory ID {die_id}")

        cursor = die_offset + 4
        for _ in range(num_ips):
            require_range(data, cursor, 8, table_end, "IP entry header")
            hw_id = u16(data, cursor, binary_size, "IP HWID")
            instance = data[cursor + 2]
            base_count = data[cursor + 3]
            major, minor, revision, flags = data[cursor + 4:cursor + 8]
            entry_size = 8 + base_count * address_width
            require_range(data, cursor, entry_size, table_end, "IP entry")
            if address_width == 8:
                raw_bases = list(struct.unpack_from(f"<{base_count}Q", data, cursor + 8))
                # This mirrors amdgpu's 64-bit discovery conversion for the
                # effective 32-bit register-base array.
                bases = [(value & 0xFFFFFFFF) & 0x3FFFFFFF for value in raw_bases]
            else:
                raw_bases = list(struct.unpack_from(f"<{base_count}I", data, cursor + 8))
                bases = raw_bases
            entries.append({
                "die": die_id,
                "hw_id": hw_id,
                "instance": instance,
                "version": (major, minor, revision),
                "flags": flags,
                "raw_bases": raw_bases,
                "bases": bases,
            })
            cursor += entry_size

    if cursor > ip_offset + table_size:
        raise ParseError("IP records extend past the discovery table")
    return binary_size, table_version, entries


def compare_sysfs(entries, bdf: str) -> None:
    root = Path("/sys/bus/pci/devices") / bdf / "ip_discovery"
    try:
        expected_count = int((root / "die/0/num_ips").read_text().strip(), 0)
    except OSError as exc:
        raise ParseError(f"cannot read sysfs discovery count for {bdf}: {exc}") from exc
    parsed = [entry for entry in entries if entry["die"] == 0]
    if len(parsed) != expected_count:
        raise ParseError(f"binary has {len(parsed)} die 0 entries; sysfs reports {expected_count}")

    for entry in parsed:
        item = root / "die" / "0" / str(entry["hw_id"]) / str(entry["instance"])
        try:
            version = tuple(int((item / part).read_text().strip(), 0)
                            for part in ("major", "minor", "revision"))
            bases = [int(value, 0) for value in (item / "base_addr").read_text().split()]
        except OSError as exc:
            raise ParseError(f"cannot read sysfs entry HWID {entry['hw_id']}: {exc}") from exc
        if version != entry["version"] or bases != entry["bases"]:
            raise ParseError(
                f"binary/sysfs mismatch for HWID {entry['hw_id']} instance {entry['instance']}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path, help="raw amdgpu_discovery blob")
    parser.add_argument("--compare-sysfs", metavar="PCI-BDF",
                        help="compare die 0 versions and bases to Linux sysfs")
    args = parser.parse_args()

    try:
        data = args.binary.read_bytes()
        binary_size, table_version, entries = parse_blob(data)
        if args.compare_sysfs:
            compare_sysfs(entries, args.compare_sysfs)
        print(f"valid discovery binary: size={binary_size} bytes, IP table v{table_version}")
        print(f"IP entries: {len(entries)}")
        for entry in sorted(entries, key=lambda item: (item["die"], item["hw_id"], item["instance"])):
            bases = " ".join(f"0x{base:08x}" for base in entry["bases"])
            print(f"  die {entry['die']} HWID {entry['hw_id']:3d} instance {entry['instance']}: "
                  f"{entry['version'][0]}.{entry['version'][1]}.{entry['version'][2]} bases {bases}")
        if args.compare_sysfs:
            print(f"sysfs comparison: all {sum(e['die'] == 0 for e in entries)} die 0 entries match")
    except (OSError, ParseError, struct.error) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
