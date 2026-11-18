# Release tools

The scripts the release workflow and local `dist/` builds use.

| script | what |
|---|---|
| `merge_image.py` | bootloader + partition table + app + `boot_app0.bin` into one image at `0x0` (M5Burner / M5Launcher / ESP Web Tools) |
| `firmware_manifest.py` | `tamagotcho-firmware.txt`, the OTA manifest, with the app-slot size check |
| `build_dist.py` | local release build: the two release environments, app-only and 0x0 images, ESP Web Tools manifests, OTA manifest, `SHA256SUMS.txt` |
| `test_*.py` | stdlib unittest for the three scripts |

## Local `dist/`

From the repository root:

```sh
make dist VERSION=vX.Y.Z
```

This runs `pio run` for `m5stack` and `tdisplay-s3`, then writes to `dist/`:

- `tamagotcho-latest.bin` — the `m5stack` image at `0x0` (M5Burner, M5Launcher, esptool)
- `tamagotcho-<version>-ota.bin` — the app-only `m5stack` image (OTA)
- `tamagotcho-tdisplay-s3.bin` — app-only image of the T-Display-S3
- `tamagotcho-tdisplay-s3-merged.bin` — first install at `0x0` for the T-Display-S3
- `tamagotcho-<env>-manifest.json` — ESP Web Tools (m5stack and tdisplay-s3)
- `boot_app0.bin` — app-only USB update record
- `tamagotcho-firmware.txt` — OTA manifest, with `sticks3`/`cardputer-adv` aliases
- `SHA256SUMS.txt`

Use `--skip-build` to merge existing `firmware/.pio/build/<env>` artifacts.

## Tests

```sh
python3 -m unittest discover -s tools/release -v
```

The tests use only the standard library and the real `platformio.ini` and
partition tables; they do not build firmware.
