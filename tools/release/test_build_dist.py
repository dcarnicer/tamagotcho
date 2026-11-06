"""Tests for build_dist.py's pure helpers and M5Burner package (stdlib unittest).

    python3 -m unittest discover -s tools/release -v
"""

from __future__ import annotations

import json
import tempfile
import unittest
import zipfile
from pathlib import Path

import build_dist as bd


class VersionTest(unittest.TestCase):
    def test_normalizes_v_prefix(self):
        self.assertEqual(bd.normalize_version("0.7.2"), "v0.7.2")
        self.assertEqual(bd.normalize_version("v0.7.2"), "v0.7.2")

    def test_rejects_other_shapes(self):
        for value in ("0.7", "v1", "v0.7.2-rc1", "main"):
            with self.subTest(value=value):
                with self.assertRaises(SystemExit):
                    bd.normalize_version(value)


class AppImageTest(unittest.TestCase):
    def test_checks_esp_magic_and_version(self):
        with tempfile.TemporaryDirectory() as tmp:
            image = Path(tmp) / "firmware.bin"
            image.write_bytes(bytes([bd.fm.ESP_IMAGE_MAGIC]) + b"v0.7.2\0")
            bd.check_image_version(image, "v0.7.2")
            with self.assertRaisesRegex(SystemExit, "does not carry"):
                bd.check_image_version(image, "v0.7.3")
            image.write_bytes(b"not an image")
            with self.assertRaisesRegex(SystemExit, "not an ESP32 app image"):
                bd.check_image_version(image, "v0.7.2")


class M5BurnerPackageTest(unittest.TestCase):
    def test_package_has_the_desktop_bin_zip_and_metadata(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            merged = root / "merged.bin"
            merged.write_bytes(bytes([bd.fm.ESP_IMAGE_MAGIC]) + b"firmware")
            cover = root / "cover.png"
            cover.write_bytes(b"png")
            readme = root / "README.md"
            readme.write_text("package readme\n", encoding="utf-8")
            license_file = root / "LICENSE"
            license_file.write_text("MIT\n", encoding="utf-8")
            template = root / "m5burner.json"
            template.write_text(
                json.dumps(
                    {
                        "name": "Tamagotcho",
                        "repository": "https://github.com/old/repo",
                        "version": "0.0.0",
                        "firmware_category": {
                            "path": "firmware",
                            "device": ["StickS3", "Cardputer ADV"],
                            "default_baud": 921600,
                        },
                    }
                ),
                encoding="utf-8",
            )

            binary, archive, package = bd.package_m5burner(
                root / "dist",
                "v0.7.2",
                merged,
                repository="dcarnicer/tamagotcho",
                cover=cover,
                template=template,
                readme=readme,
                license_file=license_file,
            )

            self.assertEqual(binary.name, "Tamagotcho-0.7.2.bin")
            self.assertTrue(binary.is_file())
            self.assertEqual(binary.read_bytes(), merged.read_bytes())
            self.assertTrue(archive.is_file())
            self.assertTrue((package / "firmware" / "Tamagotcho_0x0.bin").is_file())
            self.assertTrue((package / "cover.png").is_file())
            self.assertTrue((package / "LICENSE").is_file())
            self.assertTrue((package / "README.md").is_file())
            metadata = json.loads((package / "m5burner.json").read_text(encoding="utf-8"))
            self.assertEqual(metadata["version"], "0.7.2")
            self.assertEqual(metadata["repository"], "https://github.com/dcarnicer/tamagotcho")
            with zipfile.ZipFile(archive) as zf:
                names = set(zf.namelist())
            self.assertIn("m5burner.json", names)
            self.assertIn("firmware/Tamagotcho_0x0.bin", names)
            sums = (root / "dist" / "m5burner" / "SHA256SUMS.txt").read_text(encoding="utf-8")
            self.assertIn("Tamagotcho-0.7.2.bin", sums)
            self.assertIn("tamagotcho-0.7.2.zip", sums)


if __name__ == "__main__":
    unittest.main()
