# Release tools

The scripts the release workflow and local `dist/` builds use.

| script | what |
|---|---|
| `merge_image.py` | bootloader + partition table + app + `boot_app0.bin` into one image at `0x0` (ESP Web Tools / M5Burner) |
| `firmware_manifest.py` | `tamagotcho-firmware.txt`, the OTA manifest, with the app-slot size check |
| `build_dist.py` | local release build: all four environments, merged/app-only images, ESP Web Tools manifests, OTA manifest, `SHA256SUMS.txt` and the M5Burner package |
| `test_*.py` | stdlib unittest for the three scripts |

## Local `dist/`

From the repository root:

```sh
make dist VERSION=vX.Y.Z
```

This runs `pio run` for `sticks3`, `tdisplay-s3`, `cardputer-adv` and
`m5stack`, then writes to `dist/`:

- `tamagotcho-<env>.bin` — app-only image
- `tamagotcho-<env>-merged.bin` — first install at `0x0`
- `tamagotcho-<env>-manifest.json` — ESP Web Tools
- `boot_app0.bin` — app-only USB update record
- `tamagotcho-firmware.txt` — OTA manifest
- `SHA256SUMS.txt`
- `m5burner/` — the M5Burner package (see `m5burner/README.md`)

Use `--skip-build` to merge existing `firmware/.pio/build/<env>` artifacts, and
`make m5burner VERSION=vX.Y.Z [MERGED=path]` to package an existing merged
image without rebuilding.

## Tests

```sh
python3 -m unittest discover -s tools/release -v
```

The tests use only the standard library and the real `platformio.ini` and
partition tables; they do not build firmware.
