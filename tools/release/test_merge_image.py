"""Tests for merge_image.py's part list (stdlib unittest, no esptool needed).

    python3 -m unittest discover -s tools/release -v
"""

from __future__ import annotations

import unittest
from pathlib import Path

import merge_image as mi

REPO_PARTITIONS = Path(__file__).resolve().parents[2] / "firmware" / "partitions"


class ImagePartsTest(unittest.TestCase):
    def test_ota_tables_need_boot_app0(self):
        for table in ("sticks3_8MB.csv", "tdisplay_s3_16MB.csv"):
            with self.subTest(table=table):
                parts = mi.parse_partitions(REPO_PARTITIONS / table)
                with self.assertRaisesRegex(SystemExit, "pass --boot-app0"):
                    mi.image_parts(parts, Path("build"), None)

    def test_boot_app0_goes_to_otadata_and_the_app_to_app0(self):
        boot_app0 = Path("boot_app0.bin")
        parts = mi.image_parts(mi.parse_partitions(REPO_PARTITIONS / "sticks3_8MB.csv"),
                               Path("build"), boot_app0)
        self.assertEqual(parts, [
            (0x0, Path("build/bootloader.bin")),
            (0x8000, Path("build/partitions.bin")),
            (0x10000, Path("build/firmware.bin")),
            (0xE000, boot_app0),
        ])

    def test_missing_boot_app0_file_fails(self):
        parts = [(0xE000, Path("does-not-exist/boot_app0.bin"))]
        with self.assertRaisesRegex(SystemExit, "missing build file"):
            mi.esptool_args(parts, Path("out.bin"), "esp32s3", "dio", "80m", "8MB", "esptool")


if __name__ == "__main__":
    unittest.main()
