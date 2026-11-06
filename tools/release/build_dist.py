#!/usr/bin/env python3
"""Build Tamagotcho's release ``dist/`` locally, including the M5Burner package.

This mirrors what ``.github/workflows/release.yml`` does, but on the machine in
front of you, and adds the package M5Burner's desktop publish dialog and the
M5Stack-Firmware repository want.

Release artifacts (``dist/``):

- ``tamagotcho-<env>.bin``: app-only image for USB/OTA/M5Launcher.
- ``tamagotcho-<env>-merged.bin``: one image at ``0x0`` for a first install.
- ``tamagotcho-<env>-manifest.json``: ESP Web Tools manifest.
- ``boot_app0.bin``: the OTA boot record for app-only USB updates.
- ``tamagotcho-firmware.txt``: the OTA manifest devices read.
- ``SHA256SUMS.txt``: every top-level artifact.

M5Burner package (``dist/m5burner/``):

- ``Tamagotcho-<version>.bin``: the merged ``m5stack`` image for the Publish
  dialog.
- ``tamagotcho-<version>.zip``: ``m5burner.json`` + ``firmware/`` + docs, for
  the M5Stack-Firmware PR route.
- ``tamagotcho-<version>/``: the unpacked package.

Usage:

    python3 tools/release/build_dist.py --version v0.7.2
    python3 tools/release/build_dist.py --version v0.7.2 --skip-build
    python3 tools/release/build_dist.py --version v0.7.1 --m5burner-only \\
        --merged ~/Downloads/tangocho-m5burner/v0.7.1/tamagotcho-m5stack-merged.bin

``--skip-build`` reuses ``firmware/.pio/build/<env>``; the version string in
each image must already be ``--version``. ``--m5burner-only`` packages an
existing merged image and touches nothing else.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

import firmware_manifest as fm

REPO = Path(__file__).resolve().parents[2]
DEFAULT_PROJECT = REPO / "firmware"
DEFAULT_DIST = REPO / "dist"
M5BURNER_TEMPLATE = REPO / "m5burner" / "m5burner.json"
M5BURNER_README = REPO / "m5burner" / "PACKAGE_README.md"
M5BURNER_COVER = REPO / "docs" / "images" / "cover.png"
LICENSE = REPO / "LICENSE"

# One row per release environment; keep in step with release.yml.
RELEASE_ENVS = (
    {
        "env": "sticks3",
        "name": "StickS3",
        "partitions": "sticks3_8MB.csv",
        "flash_size": "8MB",
        "publish_boot_app0": True,
    },
    {
        "env": "tdisplay-s3",
        "name": "T-Display-S3",
        "partitions": "tdisplay_s3_16MB.csv",
        "flash_size": "16MB",
        "publish_boot_app0": False,
    },
    {
        "env": "cardputer-adv",
        "name": "Cardputer ADV",
        "partitions": "sticks3_8MB.csv",
        "flash_size": "8MB",
        "publish_boot_app0": False,
    },
    {
        "env": "m5stack",
        "name": "M5 (StickS3 + Cardputer ADV)",
        "partitions": "sticks3_8MB.csv",
        "flash_size": "8MB",
        "publish_boot_app0": False,
    },
)

VERSION_RE = re.compile(r"v?([0-9]+\.[0-9]+\.[0-9]+)")


def normalize_version(value: str) -> str:
    """``0.7.2`` or ``v0.7.2`` -> ``v0.7.2``; anything else is a usage error."""
    match = VERSION_RE.fullmatch(value)
    if not match:
        raise SystemExit(f"--version must be vMAJOR.MINOR.PATCH (got {value!r})")
    return "v" + match.group(1)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def find_boot_app0(project: Path, override: Path | None) -> Path:
    if override:
        if not override.is_file():
            raise SystemExit(f"missing --boot-app0 file: {override}")
        return override
    cores = []
    if os.environ.get("PLATFORMIO_CORE_DIR"):
        cores.append(Path(os.environ["PLATFORMIO_CORE_DIR"]))
    cores += [Path.home() / ".platformio", project / ".platformio-core", project / ".platformio"]
    for core in cores:
        candidate = core / "packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin"
        if candidate.is_file():
            return candidate
    raise SystemExit(
        "cannot find boot_app0.bin; pass --boot-app0 "
        "(framework-arduinoespressif32/tools/partitions/boot_app0.bin)"
    )


def run(command: list[str], *, cwd: Path, env: dict[str, str] | None = None) -> None:
    print("+", " ".join(str(part) for part in command))
    subprocess.run(command, cwd=cwd, env=env, check=True)


def check_image_version(app_image: Path, version: str) -> None:
    if not app_image.is_file():
        raise SystemExit(f"missing app image: {app_image}")
    data = app_image.read_bytes()
    if not data or data[0] != fm.ESP_IMAGE_MAGIC:
        raise SystemExit(f"{app_image} is not an ESP32 app image")
    if version.encode("utf-8") not in data:
        raise SystemExit(
            f"{app_image} does not carry {version}; build again or drop --skip-build"
        )


def stage_app(project: Path, dist: Path, env: str, version: str) -> Path:
    build = project / ".pio" / "build" / env / "firmware.bin"
    check_image_version(build, version)
    out = dist / f"tamagotcho-{env}.bin"
    shutil.copy2(build, out)
    print(f"app-only  {out.name:36} {out.stat().st_size:>10,} B")
    return out


def merge_image(
    project: Path,
    dist: Path,
    env: str,
    partitions: str,
    flash_size: str,
    boot_app0: Path,
    esptool: str,
) -> Path:
    out = dist / f"tamagotcho-{env}-merged.bin"
    run(
        [
            sys.executable,
            str(REPO / "tools/release/merge_image.py"),
            "--build-dir",
            str(project / ".pio" / "build" / env),
            "--partitions",
            str(project / "partitions" / partitions),
            "--flash-size",
            flash_size,
            "--boot-app0",
            str(boot_app0),
            "--esptool",
            esptool,
            "--out",
            str(out),
        ],
        cwd=REPO,
    )
    print(f"merged    {out.name:36} {out.stat().st_size:>10,} B")
    return out


def write_esp_manifest(dist: Path, env: str, name: str, version: str) -> Path:
    path = dist / f"tamagotcho-{env}-manifest.json"
    data = {
        "name": f"Tamagotcho {name}",
        "version": version[1:],
        "new_install_prompt_erase": True,
        "builds": [
            {
                "chipFamily": "ESP32-S3",
                "parts": [{"path": f"tamagotcho-{env}-merged.bin", "offset": 0}],
            }
        ],
    }
    path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    print(f"manifest  {path.name}")
    return path


def write_firmware_manifest(project: Path, dist: Path, version: str) -> Path:
    text, report = fm.build(version, project, dist)
    for line in report:
        print(line)
    out = dist / fm.MANIFEST_NAME
    out.write_bytes(text.encode("utf-8"))
    print(f"wrote     {out.name} ({len(report)} images)\n{out.read_text()}")
    return out


def write_sha256sums(dist: Path) -> Path:
    out = dist / "SHA256SUMS.txt"
    rows = []
    for path in sorted(dist.iterdir()):
        if path.is_file() and path != out:
            rows.append(f"{sha256(path)}  {path.name}")
    out.write_text("\n".join(rows) + "\n", encoding="utf-8")
    print(f"wrote     {out.name} ({len(rows)} files)")
    return out


def package_m5burner(
    dist: Path,
    version: str,
    merged: Path,
    *,
    repository: str,
    cover: Path,
    template: Path = M5BURNER_TEMPLATE,
    readme: Path = M5BURNER_README,
    license_file: Path = LICENSE,
) -> tuple[Path, Path, Path]:
    """Create the M5Burner package from a merged ``0x0`` image.

    Returns (binary for the Publish dialog, zip for the repo/PR route,
    unpacked package directory).
    """
    if not merged.is_file():
        raise SystemExit(f"missing merged image: {merged}")
    for path, label in ((template, "m5burner.json template"), (readme, "package README"),
                        (cover, "cover"), (license_file, "LICENSE")):
        if not path.is_file():
            raise SystemExit(f"missing {label}: {path}")
    slug = version[1:]
    output = dist / "m5burner"
    package = output / f"tamagotcho-{slug}"
    if package.exists():
        shutil.rmtree(package)
    (package / "firmware").mkdir(parents=True)

    metadata = json.loads(template.read_text(encoding="utf-8"))
    metadata["version"] = slug
    metadata["repository"] = f"https://github.com/{repository}"
    (package / "m5burner.json").write_text(
        json.dumps(metadata, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )
    shutil.copy2(merged, package / "firmware" / "Tamagotcho_0x0.bin")
    shutil.copy2(cover, package / "cover.png")
    shutil.copy2(readme, package / "README.md")
    shutil.copy2(license_file, package / "LICENSE")
    notes = REPO / "docs" / "releases" / f"{version}.md"
    if notes.is_file():
        shutil.copy2(notes, package / "VERSION_DESCRIPTION.md")

    archive = output / f"tamagotcho-{slug}.zip"
    if archive.exists():
        archive.unlink()
    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        for path in sorted(package.rglob("*")):
            if path.is_file():
                zf.write(path, path.relative_to(package))

    binary = output / f"Tamagotcho-{slug}.bin"
    shutil.copy2(merged, binary)
    (output / "SHA256SUMS.txt").write_text(
        f"{sha256(binary)}  {binary.name}\n{sha256(archive)}  {archive.name}\n",
        encoding="utf-8",
    )
    print(f"m5burner  {binary}")
    print(f"m5burner  {archive}")
    print(f"m5burner  {package}/")
    print(f"binary SHA256: {sha256(binary)}")
    print(f"archive SHA256: {sha256(archive)}")
    return binary, archive, package


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="build_dist", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--version", required=True, help="release tag, vMAJOR.MINOR.PATCH")
    ap.add_argument("--project", default=str(DEFAULT_PROJECT), help="PlatformIO project (firmware/)")
    ap.add_argument("--dist", default=str(DEFAULT_DIST), help="output directory (dist/)")
    ap.add_argument("--skip-build", action="store_true",
                    help="use firmware/.pio/build/<env> instead of running pio")
    ap.add_argument("--keep", action="store_true", help="do not clear dist/ first")
    ap.add_argument("--no-m5burner", action="store_true", help="skip the M5Burner package")
    ap.add_argument("--m5burner-only", action="store_true",
                    help="only package an existing merged image")
    ap.add_argument("--merged", help="merged image for --m5burner-only")
    ap.add_argument("--repository", default="dcarnicer/tamagotcho")
    ap.add_argument("--cover", default=str(M5BURNER_COVER))
    ap.add_argument("--boot-app0", help="override the framework's boot_app0.bin")
    ap.add_argument("--pio", default="pio", help="PlatformIO executable")
    ap.add_argument("--esptool", default="esptool", help="esptool executable")
    args = ap.parse_args(argv)

    version = normalize_version(args.version)
    project = Path(args.project).expanduser().resolve()
    dist = Path(args.dist).expanduser().resolve()

    if args.m5burner_only:
        merged = (Path(args.merged).expanduser().resolve() if args.merged
                  else dist / "tamagotcho-m5stack-merged.bin")
        package_m5burner(dist, version, merged, repository=args.repository,
                         cover=Path(args.cover).expanduser().resolve())
        return 0

    if dist.exists() and not args.keep:
        print(f"clearing {dist}")
        shutil.rmtree(dist)
    dist.mkdir(parents=True, exist_ok=True)
    boot_app0 = find_boot_app0(project, Path(args.boot_app0).expanduser() if args.boot_app0 else None)
    build_env = os.environ.copy()
    build_env["TGC_VERSION"] = version

    for row in RELEASE_ENVS:
        env = row["env"]
        if not args.skip_build:
            run([args.pio, "run", "-e", env], cwd=project, env=build_env)
        stage_app(project, dist, env, version)
        merge_image(project, dist, env, row["partitions"], row["flash_size"],
                    boot_app0, args.esptool)
        write_esp_manifest(dist, env, row["name"], version)
        if row["publish_boot_app0"]:
            shutil.copy2(boot_app0, dist / "boot_app0.bin")
            print(f"boot      {(dist / 'boot_app0.bin').name:36} "
                  f"{(dist / 'boot_app0.bin').stat().st_size:>10,} B")

    write_firmware_manifest(project, dist, version)
    if not args.no_m5burner:
        package_m5burner(
            dist,
            version,
            dist / "tamagotcho-m5stack-merged.bin",
            repository=args.repository,
            cover=Path(args.cover).expanduser().resolve(),
        )
    write_sha256sums(dist)
    print(f"\nDone: {dist}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
