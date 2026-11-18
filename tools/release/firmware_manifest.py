#!/usr/bin/env python3
"""Write a release's firmware manifest, ``tamagotcho-firmware.txt``.

The device reads it from the latest release and installs the row of its own
build environment (docs/ota.md, "The release side"). Format version 1, as
docs/ota.md specifies it (fields separated by one TAB, lines end in LF):

    #tamagotcho-firmware    1
    version                 v0.8.0
    app                     <env>  tamagotcho-<env>.bin  <size>  <sha256>
    app                     m5stack  tamagotcho-<version>-ota.bin  <size>  <sha256>

One ``app`` row per app-only image ``tamagotcho-<env>.bin`` in the dist
directory (the ``-merged`` and ``tamagotcho-latest.bin`` images are not app
images, and ``*-ota.bin`` is skipped unless ``--m5stack-ota`` names the
m5stack one); sorted by file name, with m5stack last. The m5stack row names
the release's ``tamagotcho-<version>-ota.bin`` asset. ``--alias ENV`` adds a
row for an older build environment that must keep updating (``sticks3`` and
``cardputer-adv``): it names the same m5stack image, so a device still on that
build installs the combined one once and then follows the m5stack row. The
file name is resolved by the device against the manifest's own URL, so the
row names the asset of the same release. The size is decimal, the SHA-256
lowercase hex.

The script fails, before anything is published, when:

- the tag is not ``v<major>.<minor>.<patch>`` (a device refuses such a
  manifest, and takes a firmware with such a version for a development build);
- an image does not start with the ESP image magic ``0xE9``;
- an image is larger than its environment's app slot. The env's partition CSV
  is its ``board_build.partitions`` in platformio.ini, followed through
  ``extends`` the way PlatformIO does; the table must have two or more app
  slots, all of one size (an update goes to whichever one is not running), and
  that size is the limit.

Usage (release.yml):

    python3 tools/release/firmware_manifest.py --version v0.8.0 \\
        --project firmware --dist dist \\
        --m5stack-ota dist/tamagotcho-0.8.0-ota.bin \\
        --alias sticks3 --alias cardputer-adv \\
        --out dist/tamagotcho-firmware.txt
"""

from __future__ import annotations

import argparse
import configparser
import hashlib
import re
import sys
from pathlib import Path

from merge_image import parse_partitions

HEADER = "#tamagotcho-firmware"
FORMAT_VERSION = 1
MANIFEST_NAME = "tamagotcho-firmware.txt"
MAX_MANIFEST_BYTES = 8 * 1024  # a longer body is not a manifest (docs/ota.md)
ESP_IMAGE_MAGIC = 0xE9

VERSION_RE = re.compile(r"v[0-9]+\.[0-9]+\.[0-9]+")
ENV_RE = re.compile(r"[a-z0-9-]{1,31}")
IMAGE_RE = re.compile(r"tamagotcho-(.+)\.bin")

DEFAULT_PROJECT = Path(__file__).resolve().parents[2] / "firmware"


class ManifestError(Exception):
    """A release that must not be published; the message says why."""


def check_version(version: str) -> str:
    if not VERSION_RE.fullmatch(version):
        raise ManifestError(
            f"release tag {version!r} is not v<major>.<minor>.<patch>: the manifest "
            "needs a plain version (docs/ota.md, manifest format version 1)")
    return version


def _multi_values(raw: str) -> list[str]:
    # PlatformIO's list syntax: one per line, or comma-separated on one line.
    items = raw.split("\n") if "\n" in raw else raw.split(",")
    return [i.strip() for i in items if i.strip() and not i.strip().startswith((";", "#"))]


def env_partitions(project: Path, env: str) -> Path:
    """The partition CSV of ``[env:<env>]``, as PlatformIO resolves it."""
    ini = project / "platformio.ini"
    # The parser PlatformIO uses, minus interpolation (${...} stays text).
    parser = configparser.ConfigParser(inline_comment_prefixes=("#", ";"), interpolation=None)
    if not parser.read(ini, encoding="utf-8"):
        raise ManifestError(f"cannot read {ini}")
    root = f"env:{env}"
    if not parser.has_section(root):
        raise ManifestError(f"{ini}: no [{root}] for tamagotcho-{env}.bin")
    # PlatformIO's walk_options(): the env, then what it extends (a stack, so
    # the last `extends` entry is searched first), then [env].
    queue, done = ["env", root], []
    while queue:
        section = queue.pop()
        if section in done:
            continue
        done.append(section)
        if not parser.has_section(section):
            continue
        if parser.has_option(section, "board_build.partitions"):
            return project / parser.get(section, "board_build.partitions").strip()
        if parser.has_option(section, "extends"):
            queue.extend(_multi_values(parser.get(section, "extends")))
    raise ManifestError(f"{ini}: [{root}] sets no board_build.partitions")


def slot_size(csv_path: Path) -> int:
    """The size of the table's app slots (two or more, all of one size)."""
    if not csv_path.is_file():
        raise ManifestError(f"no partition table {csv_path}")
    apps = [p for p in parse_partitions(csv_path) if p["type"] == "app"]
    if len(apps) < 2:
        raise ManifestError(
            f"{csv_path}: {len(apps)} app partition(s); an updatable table has two (ota_0, ota_1)")
    sizes = sorted({p["size"] for p in apps})
    if len(sizes) != 1:
        listed = ", ".join(f"{p['name']} {p['size']:#x}" for p in apps)
        raise ManifestError(
            f"{csv_path}: the app slots differ in size ({listed}); an update may go to any of them")
    return sizes[0]


def find_images(dist: Path, m5stack_ota: Path | None = None) -> list[tuple[str, Path]]:
    """(env, path) of every app-only ``tamagotcho-<env>.bin``, by file name.

    ``tamagotcho-latest.bin`` (the 0x0 image) and ``*-ota.bin`` are not app
    images by that naming rule; the latter is added explicitly by ``build()``
    when it is the m5stack update asset."""
    images = []
    for path in sorted(dist.glob("tamagotcho-*.bin")):
        if path.name == "tamagotcho-latest.bin" or path.name.endswith("-ota.bin"):
            continue
        if m5stack_ota is not None and path.name == "tamagotcho-m5stack.bin":
            continue
        env = IMAGE_RE.fullmatch(path.name).group(1)
        if env.endswith("-merged"):
            continue
        if not ENV_RE.fullmatch(env):
            raise ManifestError(f"{path.name}: {env!r} is not an environment name ([a-z0-9-]{{1,31}})")
        images.append((env, path))
    if not images:
        raise ManifestError(f"no app-only tamagotcho-<env>.bin in {dist}")
    return images


def image_digest(path: Path) -> tuple[int, str]:
    """(size, lowercase SHA-256) of an app image, after a magic-byte check."""
    data = path.read_bytes()  # ~1.5 MB
    if not data or data[0] != ESP_IMAGE_MAGIC:
        raise ManifestError(f"{path} is not an ESP32 app image (expected 0xE9 magic)")
    return len(data), hashlib.sha256(data).hexdigest()


def manifest_text(version: str, rows: list[tuple[str, str, int, str]]) -> str:
    """Format version 1 from (env, file name, size, sha256) rows."""
    lines = [f"{HEADER}\t{FORMAT_VERSION}", f"version\t{check_version(version)}"]
    for env, name, size, sha in rows:
        lines.append("\t".join(("app", env, name, str(size), sha)))
    text = "\n".join(lines) + "\n"
    if len(text.encode("utf-8")) > MAX_MANIFEST_BYTES:
        raise ManifestError(f"the manifest is over {MAX_MANIFEST_BYTES} bytes")
    return text


def build(version: str, project: Path, dist: Path,
          m5stack_ota: Path | None = None,
          aliases: list[str] | tuple[str, ...] = ()) -> tuple[str, list[str]]:
    """The manifest text and a usage report; ManifestError for any image over
    its slot (all of them listed) or any other problem.

    ``m5stack_ota`` is the app-only m5stack image under its release asset name
    (``tamagotcho-<version>-ota.bin``): the m5stack row then names that file,
    while the other envs keep ``tamagotcho-<env>.bin``. ``aliases`` are older
    environment names that get a row with the same image, for devices that
    still run that build; each alias's own partition table is checked."""
    check_version(version)
    aliases = list(aliases)
    if aliases and m5stack_ota is None:
        raise ManifestError("an alias needs the m5stack OTA image (--m5stack-ota)")
    rows, report, over = [], [], []
    images = find_images(dist, m5stack_ota)
    seen = {env for env, _ in images}
    if m5stack_ota is not None:
        if not m5stack_ota.is_file():
            raise ManifestError(f"missing m5stack OTA image: {m5stack_ota}")
        if "m5stack" in seen:
            raise ManifestError("dist has a tamagotcho-m5stack.bin and --m5stack-ota: "
                                "the m5stack row would be duplicated")
        images.append(("m5stack", m5stack_ota))
        seen.add("m5stack")
        for env in aliases:
            if not ENV_RE.fullmatch(env):
                raise ManifestError(f"alias {env!r} is not an environment name "
                                    "([a-z0-9-]{1,31})")
            if env in seen:
                raise ManifestError(f"alias {env!r} already has an app image row")
            images.append((env, m5stack_ota))
            seen.add(env)
    alias_set = set(aliases)
    for env, path in images:
        csv_path = env_partitions(project, env)
        slot = slot_size(csv_path)
        size, sha = image_digest(path)
        rows.append((env, path.name, size, sha))
        note = " [alias of m5stack]" if env in alias_set else ""
        report.append(f"{env:16} {size:>10,} B of {slot:>10,} B ({100 * size / slot:5.1f} %, "
                      f"{slot - size:,} B free) [{csv_path.name}]{note}")
        if size > slot:
            over.append(f"{path.name}: {size:,} B is larger than the {slot:,} B app slot "
                        f"of {csv_path.name} by {size - slot:,} B")
    if over:
        raise ManifestError("image larger than its app slot:\n  " + "\n  ".join(over))
    return manifest_text(version, rows), report


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="firmware_manifest", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--version", required=True, help="the release tag, e.g. v0.7.0")
    ap.add_argument("--project", default=str(DEFAULT_PROJECT),
                    help="the PlatformIO project (platformio.ini, partitions/); default: firmware/")
    ap.add_argument("--dist", required=True, help="the directory with the tamagotcho-<env>.bin images")
    ap.add_argument("--m5stack-ota", metavar="PATH",
                    help="the m5stack app-only image under its release asset name "
                         "(tamagotcho-<version>-ota.bin); its manifest row names that file")
    ap.add_argument("--alias", action="append", default=[], metavar="ENV",
                    help="add a row for an older environment (e.g. sticks3) with the "
                         "--m5stack-ota image, so that build migrates to m5stack")
    ap.add_argument("--out", help=f"manifest to write (default: <dist>/{MANIFEST_NAME})")
    a = ap.parse_args(argv)

    dist = Path(a.dist)
    out = Path(a.out) if a.out else dist / MANIFEST_NAME
    try:
        text, report = build(a.version, Path(a.project), dist,
                             Path(a.m5stack_ota) if a.m5stack_ota else None,
                             aliases=a.alias)
    except ManifestError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    for line in report:
        print(line)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(text.encode("utf-8"))  # LF line ends on every host
    print(f"wrote {out} ({len(report)} images)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
