# Tangochō – notes for agents

Kanji multiple-choice review app for the M5Stack StickS3. Read
`docs/architecture.md` and `docs/pack-format.md` before touching code.

- Repo docs and code comments in **English** (project is meant to be published).
- `docs/pack-format.md` is the contract between `tools/pack-builder` and
  `firmware`. Change the spec first, then both sides.
- Record non-obvious decisions in `docs/decisions.md` (short entries, dated).
- Firmware: PlatformIO, `cd firmware && pio run -e sticks3`, native tests
  with `pio test -e native`. Pure logic (`pack/`, `quiz/`, `srs/`, `imu/`,
  `power/`, `catalog/`, `portal/`, `ui/`) must not include Arduino headers; the
  `native` env builds and tests it.
- Pack builder: Python managed with `uv` (`cd tools/pack-builder && uv run tamagotcho-pack --help`).
- Downloaded data goes to `tools/pack-builder/cache/` (git-ignored), never committed.
- Data licences: KANJIDIC2 and KanjiVG are CC BY-SA; generated packs inherit
  that licence. Code is MIT. Keep `docs/data-sources.md` up to date.
