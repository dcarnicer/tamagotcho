# M5Burner package

This directory holds the M5Burner metadata template and the publishing
checklist. Generated packages go to `dist/m5burner/` (ignored by git).

## Build the package

From the repository root:

```sh
make dist VERSION=v0.7.2
```

This builds the four release environments, writes the merged and app-only
images, `tamagotcho-firmware.txt` and the ESP Web Tools manifests to `dist/`,
and creates the M5Burner package under `dist/m5burner/`. The GitHub release
workflow also builds this package and attaches `Tamagotcho-<version>.bin` and
`tamagotcho-m5burner-<version>.zip` to every release.

To package an existing merged image without rebuilding (for example the
released v0.7.1 asset):

```sh
make m5burner VERSION=v0.7.1 \
  MERGED=~/Downloads/tangocho-m5burner/v0.7.1/tamagotcho-m5stack-merged.bin
```

The script produces:

```
dist/m5burner/
├── Tamagotcho-<version>.bin          # for the M5Burner Publish dialog
├── tamagotcho-<version>.zip          # for the M5Stack-Firmware PR route
├── SHA256SUMS.txt
└── tamagotcho-<version>/
    ├── m5burner.json
    ├── LICENSE
    ├── README.md
    ├── VERSION_DESCRIPTION.md        # when docs/releases/<version>.md exists
    ├── cover.png
    └── firmware/
        └── Tamagotcho_0x0.bin
```

The `.bin` and `firmware/Tamagotcho_0x0.bin` are the merged `m5stack` image
(bootloader + partition table + boot record + app) written at `0x0`. Use the
`.bin` in the M5Burner desktop **Publish** dialog; use the `.zip` only for the
`M5Stack-Firmware` repository/PR route.

The entry is one image for the **StickS3** and the **Cardputer ADV** (the
board is found at boot). Do **not** upload `tamagotcho-m5stack.bin`: that is
the app-only image for `0x10000`, and M5Burner writes its file at `0x0`.

## Test before publishing

1. Build and package from a clean tree, or use the merged image from the
   GitHub release.
2. On a real StickS3 and/or Cardputer ADV, erase the flash and write the
   package's `Tamagotcho_0x0.bin` at `0x0`.
3. Check that it boots. M5Stack rejects a firmware that does not (v0.7.0 was
   rejected with that reason on 2026-11-06; the merged image was later tested
   and boots, so check the attached file first).
4. A device coming from v0.6.0 or older needs a fresh install: the v0.7.0
   flash layout moves the decks' storage. Back up the progress on the portal
   first (Advanced > Progress backup), then download the decks again and
   restore it.

## Publish

Use your own M5Stack account. There are two supported routes:

1. **M5Burner desktop app**: `USER CUSTOM` → `Publish`, fill name, version,
   version description, device type (StickS3 and Cardputer ADV), GitHub URL,
   the `Tamagotcho-<version>.bin` firmware and `cover.png`.
2. **M5Stack-Firmware PR**: fork `m5stack/M5Stack-Firmware`, add this
   repository to `firmware-repo.list`, and include `m5burner.json` plus the
   `firmware/` folder in this repository.

The version description is `docs/releases/<version>.md` (plus the fresh-install
warning in `docs/m5burner.md` when it applies). The shared listing
description lives in `docs/m5burner.md`.

## Legal checklist

- [ ] Release tag exists for the exact firmware binary.
- [ ] `LICENSE` is included (code: MIT).
- [ ] No decks are shipped in the package. Decks are CC BY-SA and live in the
      `decks-v<pack format>` release; the device downloads them over Wi-Fi.
- [ ] No M5Stack logo or "official firmware" claim is used.
- [ ] M5Stack terms and review requirements are accepted.

## Notes

- `m5burner.json` here is a template; the packaging script replaces `version`
  and normalizes `repository`.
- `m5burner.json`'s `version` is the tag without the leading `v` (`0.7.2`);
  the M5Burner form's version field uses the tag (`v0.7.2`).
- The cover is `docs/images/cover.png` by default.
- The package's `README.md` is `m5burner/PACKAGE_README.md`;
  `VERSION_DESCRIPTION.md` is a copy of the release notes when they exist.
