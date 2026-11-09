#!/usr/bin/env python3
"""Put a FIT bootkernel in MX84 slot 1 without touching recovery regions."""

from __future__ import annotations

import argparse
import hashlib
import struct
from pathlib import Path


ROM_SIZE = 0x1000000
DESCRIPTOR_SIZE = 0x10000
BOOTKERNEL_1_OFFSET = 0x10000
BOOTKERNEL_SIZE = 0x700000
BOOTKERNEL_1_END = BOOTKERNEL_1_OFFSET + BOOTKERNEL_SIZE
FIT_MAGIC = 0xD00DFEED
KNOWN_DESCRIPTOR_SHA256 = (
    "9308f3938c4291ea8b0ae70be3720e8a3446cd0d0f5cc31c157bec56cc2bd9f8"
)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def load_stock(path: Path) -> bytes:
    data = path.read_bytes()
    if len(data) != ROM_SIZE:
        raise ValueError(f"stock ROM is {len(data)} bytes; expected {ROM_SIZE}")
    descriptor_hash = sha256(data[:DESCRIPTOR_SIZE])
    if descriptor_hash != KNOWN_DESCRIPTOR_SHA256:
        raise ValueError(
            "stock ROM has an unknown Intel descriptor: " + descriptor_hash
        )
    return data


def load_fit(path: Path) -> bytes:
    data = path.read_bytes()
    if len(data) < 8:
        raise ValueError("bootkernel is too short to be a FIT image")
    magic, total_size = struct.unpack(">II", data[:8])
    if magic != FIT_MAGIC:
        raise ValueError(f"bootkernel has FIT magic 0x{magic:08x}; expected 0xd00dfeed")
    if total_size > len(data):
        raise ValueError(
            f"FIT header declares {total_size} bytes, but file has only {len(data)}"
        )
    if total_size > BOOTKERNEL_SIZE:
        raise ValueError(
            f"FIT is 0x{total_size:x} bytes; MX84 slot limit is 0x{BOOTKERNEL_SIZE:x}"
        )
    if len(data) > BOOTKERNEL_SIZE:
        raise ValueError(
            f"bootkernel file is 0x{len(data):x} bytes; slot limit is 0x{BOOTKERNEL_SIZE:x}"
        )
    if any(byte != 0xFF for byte in data[total_size:]):
        raise ValueError("bytes after the FIT total size are not erased padding (0xff)")
    return data[:total_size]


def make_image(stock: bytes, fit: bytes) -> bytes:
    result = bytearray(stock)
    result[BOOTKERNEL_1_OFFSET:BOOTKERNEL_1_END] = b"\xff" * BOOTKERNEL_SIZE
    result[BOOTKERNEL_1_OFFSET:BOOTKERNEL_1_OFFSET + len(fit)] = fit
    output = bytes(result)
    if output[:BOOTKERNEL_1_OFFSET] != stock[:BOOTKERNEL_1_OFFSET]:
        raise AssertionError("descriptor changed")
    if output[BOOTKERNEL_1_END:] != stock[BOOTKERNEL_1_END:]:
        raise AssertionError("recovery bootkernel or coreboot changed")
    return output


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stock_rom", type=Path)
    parser.add_argument("bootkernel_fit", type=Path)
    parser.add_argument("output_rom", type=Path)
    parser.add_argument(
        "--force", action="store_true", help="replace an existing output file"
    )
    args = parser.parse_args()

    if args.output_rom.exists() and not args.force:
        parser.error(f"output already exists: {args.output_rom} (use --force)")

    try:
        stock = load_stock(args.stock_rom)
        fit = load_fit(args.bootkernel_fit)
        output = make_image(stock, fit)
    except (OSError, ValueError) as error:
        parser.error(str(error))

    args.output_rom.write_bytes(output)
    print(f"stock:  {sha256(stock)}  {args.stock_rom}")
    print(f"FIT:    {sha256(fit)}  {args.bootkernel_fit} (0x{len(fit):x} bytes)")
    print(f"output: {sha256(output)}  {args.output_rom}")
    print("preserved: descriptor, bootkernel 2, and coreboot/CBFS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
