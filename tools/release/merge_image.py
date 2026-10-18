#!/usr/bin/env python3
"""Merge a PlatformIO ESP32-S3 build into one flashable image.

`esptool merge_bin` combines the second-stage bootloader, the partition table
and the application (each at its own offset) into a single file. That is what
ESP Web Tools needs: a browser flasher writes one image at offset 0 rather than
several images at several offsets. The result mirrors what `pio run -t upload`
does for the same environment, so the device sees an identical flash layout.

Offsets come from the partition CSV (authoritative), not from constants:

- ESP32-S3 boots the second-stage bootloader from ``0x0`` and reads the partition
  table from ``0x8000``.
- the application goes to the offset of the table's ``app`` partition.
- an Arduino/ESP-IDF build with an **OTA data partition** also needs the
  ``boot_app0.bin`` image (the "which OTA slot to boot" record) at that
  partition's offset. Our tables have no OTA partition, so this part is omitted.
  (PlatformIO's USB upload still writes ``boot_app0.bin`` at ``0xe000``, over
  the unused ``phy_init`` partition; see docs/decisions.md.)

Usage (see README.md "Releases"):

    python3 tools/release/merge_image.py \\
        --build-dir .pio/build/sticks3 \\
        --partitions firmware/partitions/sticks3_8MB.csv \\
        --flash-size 8MB --out dist/tangocho-sticks3-merged.bin
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

# ESP32-S3 flash layout constants (bootloader and partition table).
BOOTLOADER_OFFSET = 0x0
PARTITION_TABLE_OFFSET = 0x8000


def parse_partitions(csv_path: Path) -> list[dict]:
    """Parse a PlatformIO partition CSV into dicts with int offsets/sizes."""
    partitions = []
    for lineno, line in enumerate(csv_path.read_text(encoding="utf-8").splitlines(), 1):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        fields = [f.strip() for f in line.split(",")]
        if len(fields) < 5:
            raise SystemExit(f"{csv_path}:{lineno}: expected 5 fields, got {len(fields)}")
        name, ptype, subtype, offset, size = fields[:5]
        partitions.append(
            {
                "name": name,
                "type": ptype,
                "subtype": subtype,
                "offset": int(offset, 0),
                "size": int(size, 0),
            }
        )
    return partitions


def image_parts(partitions: list[dict], build_dir: Path, boot_app0: Path | None) -> list[tuple[int, Path]]:
    """The (offset, file) list to merge for this partition table."""
    app = next((p for p in partitions if p["type"] == "app"), None)
    if app is None:
        raise SystemExit("no app partition in the partition table")
    parts = [
        (BOOTLOADER_OFFSET, build_dir / "bootloader.bin"),
        (PARTITION_TABLE_OFFSET, build_dir / "partitions.bin"),
        (app["offset"], build_dir / "firmware.bin"),
    ]
    ota = next((p for p in partitions if p["type"] == "data" and p["subtype"] == "ota"), None)
    if ota is not None:
        if boot_app0 is None:
            raise SystemExit(
                "the partition table has an OTA data partition; pass --boot-app0 "
                "(framework-arduinoespressif32/tools/partitions/boot_app0.bin)"
            )
        parts.append((ota["offset"], boot_app0))
    return parts


def esptool_args(parts, out: Path, chip: str, flash_mode: str, flash_freq: str,
                 flash_size: str, esptool: str) -> list[str]:
    args = [esptool, "--chip", chip, "merge_bin", "-o", str(out),
            "--flash_mode", flash_mode, "--flash_freq", flash_freq, "--flash_size", flash_size]
    for offset, path in parts:
        if not path.exists():
            raise SystemExit(f"missing build file: {path}")
        args += [hex(offset), str(path)]
    return args


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="merge_image", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build-dir", required=True, help=".pio/build/<env>")
    ap.add_argument("--partitions", required=True, help="firmware/partitions/*.csv")
    ap.add_argument("--out", required=True, help="merged image to write")
    ap.add_argument("--chip", default="esp32s3")
    # DIO, as `pio run -t upload` writes it (`-v` shows `--flash_mode dio` for
    # these qio boards): the bootloader header stays at the safe mode and the
    # app's own flash config switches to QIO.
    ap.add_argument("--flash-mode", default="dio")
    ap.add_argument("--flash-freq", default="80m")
    ap.add_argument("--flash-size", default="8MB")
    ap.add_argument("--boot-app0", help="only used when the table has an OTA data partition")
    ap.add_argument("--esptool", default="esptool")
    a = ap.parse_args(argv)

    build_dir, out = Path(a.build_dir), Path(a.out)
    partitions = parse_partitions(Path(a.partitions))
    parts = image_parts(partitions, build_dir, Path(a.boot_app0) if a.boot_app0 else None)
    args = esptool_args(parts, out, a.chip, a.flash_mode, a.flash_freq, a.flash_size, a.esptool)

    # The first byte of the app image is the ESP image magic; read just that byte
    # (the image is ~1 MB) to catch a truncated/empty firmware.bin before merging.
    app = next(p for p in parts if p[1].name == "firmware.bin")[1]
    with app.open("rb") as f:
        magic = f.read(1)
    if magic != b"\xE9":
        raise SystemExit(f"{app} is not an ESP32 app image (expected 0xE9 magic)")
    if not any(p["type"] == "data" and p["subtype"] == "ota" for p in partitions):
        print("note: no OTA data partition in the table -> no boot_app0 image (no OTA)")

    out.parent.mkdir(parents=True, exist_ok=True)
    print("+", " ".join(args))
    subprocess.run(args, check=True)
    print(f"wrote {out} ({out.stat().st_size:,} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
