"""Tests for firmware_manifest.py (stdlib unittest, no build needed).

    python3 -m unittest discover -s tools/release -v
"""

from __future__ import annotations

import hashlib
import io
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

import firmware_manifest as fm

REPO_FIRMWARE = Path(__file__).resolve().parents[2] / "firmware"

INI = """\
; a comment
[common]
build_flags = -Isrc

[env:base]
extends = common
board_build.partitions = partitions/eight.csv  ; inline comment
build_src_flags = -DTGC_ENV=\\"${this.__env__}\\"

[env:base-dev]
extends = env:base
build_flags =
    ${env:base.build_flags}
    -DX=1

[env:big]
extends = common
board_build.partitions = partitions/sixteen.csv

[env:single]
board_build.partitions = partitions/single.csv

[env:uneven]
board_build.partitions = partitions/uneven.csv

[env:nothing]
extends = common
"""

TABLES = {
    "eight.csv": """\
# Name, Type, SubType, Offset, Size, Flags
nvs,     data, nvs,    0x9000,   0x5000,
otadata, data, ota,    0xE000,   0x2000,
app0,    app,  ota_0,  0x10000,  0x1000,
app1,    app,  ota_1,  0x11000,  0x1000,
spiffs,  data, spiffs, 0x12000,  0x6E000,
""",
    "sixteen.csv": """\
otadata, data, ota,    0xE000,   0x2000,
app0,    app,  ota_0,  0x10000,  0x2000,
app1,    app,  ota_1,  0x12000,  0x2000,
""",
    "single.csv": """\
app0,    app,  factory, 0x10000, 0x200000,
""",
    "uneven.csv": """\
otadata, data, ota,    0xE000,   0x2000,
app0,    app,  ota_0,  0x10000,  0x1000,
app1,    app,  ota_1,  0x11000,  0x2000,
""",
}


def app_image(size: int, fill: int = 0x42) -> bytes:
    return bytes([fm.ESP_IMAGE_MAGIC]) + bytes([fill]) * (size - 1)


class ManifestTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        root = Path(self._tmp.name)
        self.project = root / "firmware"
        (self.project / "partitions").mkdir(parents=True)
        (self.project / "platformio.ini").write_text(INI, encoding="utf-8")
        for name, text in TABLES.items():
            (self.project / "partitions" / name).write_text(text, encoding="utf-8")
        self.dist = root / "dist"
        self.dist.mkdir()

    def tearDown(self):
        self._tmp.cleanup()

    def put(self, name: str, data: bytes) -> bytes:
        (self.dist / name).write_bytes(data)
        return data

    def run_main(self, *args: str) -> tuple[int, str, str]:
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            code = fm.main(list(args))
        return code, out.getvalue(), err.getvalue()

    def test_writes_format_version_1(self):
        base = self.put("tamagotcho-base.bin", app_image(0x1000))
        big = self.put("tamagotcho-big.bin", app_image(300, 0x00))
        self.put("tamagotcho-base-merged.bin", b"\xE9merged")  # not an app image
        self.put("tamagotcho-base-manifest.json", b"{}")
        self.put("boot_app0.bin", b"\xff" * 16)
        code, _, err = self.run_main("--version", "v0.7.0", "--project", str(self.project),
                                     "--dist", str(self.dist))
        self.assertEqual(code, 0, err)
        raw = (self.dist / fm.MANIFEST_NAME).read_bytes()
        self.assertEqual(raw.decode("utf-8"), (
            "#tamagotcho-firmware\t1\n"
            "version\tv0.7.0\n"
            f"app\tbase\ttamagotcho-base.bin\t4096\t{hashlib.sha256(base).hexdigest()}\n"
            f"app\tbig\ttamagotcho-big.bin\t300\t{hashlib.sha256(big).hexdigest()}\n"))
        self.assertNotIn(b"\r", raw)

    def test_out_path(self):
        self.put("tamagotcho-base.bin", app_image(16))
        out = self.dist / "elsewhere" / "m.txt"
        code, _, err = self.run_main("--version", "v1.2.3", "--project", str(self.project),
                                     "--dist", str(self.dist), "--out", str(out))
        self.assertEqual(code, 0, err)
        self.assertTrue(out.read_text(encoding="utf-8").startswith("#tamagotcho-firmware\t1\n"))

    def test_image_exactly_the_slot_fits(self):
        self.put("tamagotcho-base.bin", app_image(0x1000))
        text, report = fm.build("v0.7.0", self.project, self.dist)
        self.assertIn("\t4096\t", text)
        self.assertIn("100.0 %", report[0])

    def test_image_over_the_slot_fails_and_names_every_one(self):
        self.put("tamagotcho-base.bin", app_image(0x1001))
        self.put("tamagotcho-big.bin", app_image(0x2001))
        code, _, err = self.run_main("--version", "v0.7.0", "--project", str(self.project),
                                     "--dist", str(self.dist))
        self.assertEqual(code, 1)
        self.assertIn("tamagotcho-base.bin: 4,097 B is larger than the 4,096 B app slot", err)
        self.assertIn("tamagotcho-big.bin: 8,193 B", err)
        self.assertFalse((self.dist / fm.MANIFEST_NAME).exists())

    def test_derived_env_takes_the_partitions_through_extends(self):
        self.assertEqual(fm.env_partitions(self.project, "base-dev"),
                         self.project / "partitions" / "eight.csv")

    def test_env_without_partitions_or_section_fails(self):
        with self.assertRaisesRegex(fm.ManifestError, "sets no board_build.partitions"):
            fm.env_partitions(self.project, "nothing")
        with self.assertRaisesRegex(fm.ManifestError, r"no \[env:missing\]"):
            fm.env_partitions(self.project, "missing")

    def test_table_needs_two_equal_app_slots(self):
        with self.assertRaisesRegex(fm.ManifestError, "an updatable table has two"):
            fm.slot_size(self.project / "partitions" / "single.csv")
        with self.assertRaisesRegex(fm.ManifestError, "differ in size"):
            fm.slot_size(self.project / "partitions" / "uneven.csv")
        self.assertEqual(fm.slot_size(self.project / "partitions" / "sixteen.csv"), 0x2000)

    def test_bad_magic_fails(self):
        self.put("tamagotcho-base.bin", b"\x00" * 16)
        with self.assertRaisesRegex(fm.ManifestError, "not an ESP32 app image"):
            fm.build("v0.7.0", self.project, self.dist)

    def test_empty_image_fails(self):
        self.put("tamagotcho-base.bin", b"")
        with self.assertRaisesRegex(fm.ManifestError, "not an ESP32 app image"):
            fm.build("v0.7.0", self.project, self.dist)

    def test_no_images_fails(self):
        self.put("tamagotcho-base-merged.bin", app_image(16))
        with self.assertRaisesRegex(fm.ManifestError, "no app-only"):
            fm.build("v0.7.0", self.project, self.dist)

    def test_bad_env_name_fails(self):
        self.put("tamagotcho-Base.bin", app_image(16))
        with self.assertRaisesRegex(fm.ManifestError, "not an environment name"):
            fm.build("v0.7.0", self.project, self.dist)

    def test_version_must_be_plain(self):
        self.put("tamagotcho-base.bin", app_image(16))
        for bad in ("0.7.0", "v0.7", "v0.7.0-rc1", "v0.7.0-3-gabc1234", "dev", "v0.7.0\n", ""):
            with self.subTest(version=bad):
                with self.assertRaisesRegex(fm.ManifestError, "not v<major>"):
                    fm.build(bad, self.project, self.dist)
        for good in ("v0.7.0", "v10.0.12"):
            with self.subTest(version=good):
                fm.build(good, self.project, self.dist)

    def test_manifest_size_cap(self):
        rows = [(f"e{i}", f"tamagotcho-e{i}.bin", 1, "0" * 64) for i in range(200)]
        with self.assertRaisesRegex(fm.ManifestError, "over 8192 bytes"):
            fm.manifest_text("v0.7.0", rows)


class RepoTablesTest(unittest.TestCase):
    """The repository's own release envs and tables (docs/ota.md, section 2)."""

    def test_release_envs_slot_sizes(self):
        expected = {
            "sticks3": 0x1C0000,
            "cardputer-adv": 0x1C0000,
            "m5stack": 0x1C0000,
            "tdisplay-s3": 0x200000,
        }
        for env, size in expected.items():
            with self.subTest(env=env):
                self.assertEqual(fm.slot_size(fm.env_partitions(REPO_FIRMWARE, env)), size)

    def test_derived_envs_share_their_board_table(self):
        for env, base in (("sticks3-shots", "sticks3"), ("m5stack-shots", "m5stack"),
                          ("cardputer-adv-shots", "cardputer-adv"),
                          ("tdisplay-s3-shots", "tdisplay-s3")):
            with self.subTest(env=env):
                self.assertEqual(fm.env_partitions(REPO_FIRMWARE, env),
                                 fm.env_partitions(REPO_FIRMWARE, base))


if __name__ == "__main__":
    unittest.main()
