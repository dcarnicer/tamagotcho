"""Tests for build_dist.py's pure helpers (stdlib unittest).

    python3 -m unittest discover -s tools/release -v
"""

from __future__ import annotations

import tempfile
import unittest
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


class AssetNameTest(unittest.TestCase):
    def test_m5stack_app_image_is_the_versioned_ota_asset(self):
        self.assertEqual(bd.app_name("m5stack", "v0.8.0"), "tamagotcho-0.8.0-ota.bin")

    def test_other_boards_keep_their_env_name(self):
        self.assertEqual(bd.app_name("tdisplay-s3", "v0.8.0"),
                         "tamagotcho-tdisplay-s3.bin")

    def test_m5stack_merged_image_is_latest(self):
        self.assertEqual(bd.merged_name("m5stack"), "tamagotcho-latest.bin")

    def test_other_boards_keep_their_merged_name(self):
        self.assertEqual(bd.merged_name("tdisplay-s3"),
                         "tamagotcho-tdisplay-s3-merged.bin")

    def test_release_builds_the_two_platforms_and_aliases_the_old_m5_envs(self):
        self.assertEqual([row["env"] for row in bd.RELEASE_ENVS],
                         ["m5stack", "tdisplay-s3"])
        self.assertEqual(bd.LEGACY_M5_ENVS, ("sticks3", "cardputer-adv"))


if __name__ == "__main__":
    unittest.main()
