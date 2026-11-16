#!/usr/bin/env python3
"""Build and verify a local Atmosphere 1.12.0 network-only boot image."""

import argparse
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import struct


ARTIFACTS = Path(__file__).resolve().parent / ".artifacts"
PACKAGE_SIZE = 0x800000
STOCK_SHA256 = "3cc9d6ca5688e403c36974bced1089397ef8b36a4252f720207ae3d41a6eb557"
KIP_START = 0x100000
KIP_END = 0x400000
MODULE = "ams_mitm"


@dataclass(frozen=True)
class Entry:
    name: str
    table_offset: int
    offset: int
    size: int
    data: bytes


def require(condition, detail):
    if not condition:
        raise ValueError(detail)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def u32(data, offset):
    return struct.unpack_from("<I", data, offset)[0]


def read_package(image):
    require(len(image) == PACKAGE_SIZE and image[:4] == b"PK31" and image[0x20:0x24] == b"FSS0",
            "Unexpected package header")
    require(u32(image, 0x24) == PACKAGE_SIZE and u32(image, 0x2C) == 0x40,
            "Unexpected Atmosphere package table location")
    count = u32(image, 0x30)
    require(0 < count <= 30, "Invalid content table count")
    entries = []
    for row in range(count):
        table = 0x40 + row * 0x20
        offset, size = struct.unpack_from("<II", image, table)
        name = image[table + 16:table + 32].split(b"\0", 1)[0].decode("ascii")
        require(name and 0x800 <= offset < offset + size <= len(image), "Invalid content span")
        entries.append(Entry(name, table, offset, size, image[offset:offset + size]))

    require(len({e.name for e in entries}) == count, "Duplicate content name")
    ordered = sorted(entries, key=lambda entry: entry.offset)
    require(all(a.offset + a.size <= b.offset for a, b in zip(ordered, ordered[1:])), "Overlapping components")

    kips = [entry for entry in entries if KIP_START <= entry.offset < KIP_END]
    require(len(kips) == u32(image, 0x10) + 1 and kips[0].name == "emummc", "KIP count/ordering mismatch")
    for index, entry in enumerate(kips):
        meta = 0x400 + index * 48
        require(entry.offset + entry.size <= KIP_END and entry.data[:4] == b"KIP1", "Invalid KIP payload")
        require(image[meta:meta + 8] == entry.data[16:24] and
                struct.unpack_from("<II", image, meta + 8) == (entry.offset - KIP_START, entry.size) and
                image[meta + 16:meta + 48] == hashlib.sha256(entry.data).digest(), "KIP metadata mismatch")
    return entries, kips


def replace_module(stock, replacement):
    require(sha256(stock) == STOCK_SHA256, "Input does not match the confirmed official AMS 1.12.0 package")
    entries, kips = read_package(stock)
    original = kips[-1]
    require(original.name == MODULE and original.data[:12] == b"KIP1ams.mitm", "Expected ams_mitm as terminal KIP")
    require(len(replacement) >= 0x100 and replacement[:0x20] == original.data[:0x20] and
            replacement[0x80:0x100] == original.data[0x80:0x100], "Replacement KIP identity or capabilities differ")
    require(0x100 + sum(u32(replacement, offset) for offset in (0x28, 0x38, 0x48)) == len(replacement),
            "Replacement KIP segment sizes do not match its file length")

    start = original.offset
    old_end = start + original.size
    new_end = start + len(replacement)
    require(max(old_end, new_end) <= KIP_END and all(byte == 0xCC for byte in stock[old_end:KIP_END]),
            "Terminal KIP does not have canonical unused space")
    packed = bytearray(stock)
    packed[start:max(old_end, new_end)] = replacement + b"\xCC" * max(0, old_end - new_end)
    struct.pack_into("<I", packed, original.table_offset + 4, len(replacement))
    meta = 0x400 + (len(kips) - 1) * 48
    struct.pack_into("<I", packed, meta + 12, len(replacement))
    packed[meta + 16:meta + 48] = hashlib.sha256(replacement).digest()
    result = bytes(packed)

    after, _ = read_package(result)
    require([e.name for e in after] == [e.name for e in entries], "Content names changed")
    manifest = []
    for before, current in zip(entries, after):
        require(before.offset == current.offset and before.table_offset == current.table_offset,
                "A content offset changed")
        if before.name == MODULE:
            require(current.data == replacement, "Replacement readback differs")
        else:
            require(current.data == before.data, f"Unexpected change to {before.name}")
        manifest.append({"name": current.name, "sha256": sha256(current.data),
                         "bytes": current.size, "same_as_stock": current.data == before.data})

    allowed = [(original.table_offset + 4, original.table_offset + 8),
               (meta + 12, meta + 48), (start, max(old_end, new_end))]
    cursor = 0
    for lower, upper in sorted(allowed):
        require(result[cursor:lower] == stock[cursor:lower], "Bytes changed outside the permitted ranges")
        cursor = upper
    require(result[cursor:] == stock[cursor:], "Trailing bytes changed outside the permitted ranges")
    return result, manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stock", type=Path, help="official, locally copied AMS 1.12.0 package3")
    parser.add_argument("kip", type=Path, help="locally built private-network ams_mitm KIP")
    args = parser.parse_args()
    stock, kip = args.stock.read_bytes(), args.kip.read_bytes()
    packed, contents = replace_module(stock, kip)
    ARTIFACTS.mkdir(mode=0o700, exist_ok=True)
    output = ARTIFACTS / "package3-roomcircuit-native-19.0.1-stage"
    output.write_bytes(packed)
    require(output.read_bytes() == packed, "Local readback differs")
    receipt = {"stage": "local build only", "stock_sha256": sha256(stock), "kip_sha256": sha256(kip),
               "candidate_sha256": sha256(packed), "candidate_bytes": len(packed), "contents": contents,
               "outside_kip_and_its_metadata_byte_identical": True, "physical_acceptance": False}
    (ARTIFACTS / "package3-receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(f"PASS {output.name} {sha256(packed)} ({len(contents) - 1} unmodified components)")


if __name__ == "__main__":
    main()
