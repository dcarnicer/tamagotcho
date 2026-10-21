# /// script
# requires-python = ">=3.11"
# dependencies = ["Pillow>=10.3"]
# ///
"""Build the README's pictures from screen shots taken with remote.py.

    uv run tools/screenshots/readme_images.py SHOTS_DIR [-o docs/images]

SHOTS_DIR holds the 1x shots (135x240 or 240x135 from a StickS3 or a
Cardputer ADV, 170x320 from a T-Display-S3) named as the README uses them:
quiz-meaning.png, detail.png, ...; portal.png is a phone-width screenshot of
the portal page at 2x, used as it is. Each one is written 3x larger (nearest neighbour, so the pixels stay
sharp) with rounded corners; `cover.png` (1280x720, also M5Burner's cover
size) puts two of them beside the name, and `cover-cardputer.png` does the
same with two Cardputer ADV screens (its M5Burner entry). The cover's text uses Noto Sans JP
from the pack builder's cache (`uv run tamagotcho-pack fetch` downloads it).
"""

import argparse
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[2]
FONT_DIR = ROOT / "tools/pack-builder/cache/noto"

SCALE = 3
RADIUS = 18  # corner radius of a scaled screen, px

# The shots the README shows, in its order; a missing one is skipped.
SHOTS = [
    "quiz-meaning", "quiz-reading", "quiz-kanji", "feedback", "detail",
    "settings", "get-decks", "stats", "done", "quiz-landscape",
    "board-sticks3", "board-cardputer", "board-tdisplay", "portal",
]
# Browser screenshots (already at 2x): rounded corners only.
UNSCALED = {"portal"}
COVER_SCREENS = ["quiz-meaning", "detail"]
# The Cardputer ADV entry's cover: its landscape screens, one above the other.
CARDPUTER_SCREENS = ["cardputer-meaning", "cardputer-kanji"]

BG_TOP = (14, 17, 28)
BG_BOTTOM = (28, 34, 56)
BEZEL = (44, 49, 62)
TEXT = (240, 242, 247)
TEXT_DIM = (160, 168, 186)
ACCENT = (59, 110, 255)


def rounded(img: Image.Image, radius: int) -> Image.Image:
    out = img.convert("RGBA")
    mask = Image.new("L", out.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, out.width - 1, out.height - 1), radius, fill=255)
    out.putalpha(mask)
    return out


def scaled(img: Image.Image, scale: int) -> Image.Image:
    return img.convert("RGB").resize((img.width * scale, img.height * scale), Image.NEAREST)


def font(name: str, size: int) -> ImageFont.FreeTypeFont:
    path = FONT_DIR / name
    if not path.exists():
        sys.exit(f"{path} is missing: run `uv run tamagotcho-pack fetch` in tools/pack-builder")
    return ImageFont.truetype(str(path), size)


def cover(shots: Path, names: list[str] = COVER_SCREENS, board: str = "") -> Image.Image:
    w, h = 1280, 720
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        t = y / (h - 1)
        row = tuple(round(a + (b - a) * t) for a, b in zip(BG_TOP, BG_BOTTOM))
        for x in range(w):
            px[x, y] = row
    canvas = img.convert("RGBA")
    draw = ImageDraw.Draw(canvas)

    # The screens at 2x in device bezels: portrait ones side by side, the
    # second a little lower; landscape ones stacked, the second a little right.
    pad, gap = 14, 28
    shots_2x = [rounded(scaled(Image.open(shots / f"{n}.png"), 2), 10) for n in names]
    sw, sh = shots_2x[0].size
    n = len(shots_2x)
    if sw < sh:
        x0, y0, dx, dy = w - 56 - n * (sw + 2 * pad) - (n - 1) * gap, 84, sw + 2 * pad + gap, 48
    else:
        x0, y0, dx, dy = w - 56 - (sw + 2 * pad) - 40, 70, 40, sh + 2 * pad + gap
    for i, screen in enumerate(shots_2x):
        x = x0 + i * dx if sw < sh else x0 + (i % 2) * dx
        y = y0 + (i % 2) * dy if sw < sh else y0 + i * dy
        draw.rounded_rectangle((x, y, x + sw + 2 * pad, y + sh + 2 * pad), 34, fill=BEZEL)
        canvas.alpha_composite(screen, (x + pad, y + pad))

    title = font("NotoSansJP-Medium.otf", 76)
    latin = font("NotoSansJP-Medium.otf", 46)
    body = font("NotoSansJP-Regular.otf", 27)
    small = font("NotoSansJP-Regular.otf", 21)
    tx = 64
    draw.text((tx, 140), "たまごっちょ", font=title, fill=TEXT)
    draw.text((tx, 252), "Tamagotcho", font=latin, fill=TEXT)
    draw.rectangle((tx, 336, tx + 72, 341), fill=ACCENT)
    draw.text((tx, 368), "Kanji flashcards with", font=body, fill=TEXT_DIM)
    draw.text((tx, 404), "spaced repetition, offline.", font=body, fill=TEXT_DIM)
    draw.text((tx, 470), "JLPT N5–N1 · Jōyō", font=body, fill=TEXT_DIM)
    draw.text((tx, 506), "English · Español", font=body, fill=TEXT_DIM)
    draw.text((tx, 616), board or "M5Stack StickS3 · Cardputer ADV · T-Display-S3", font=small,
              fill=TEXT_DIM)
    return canvas.convert("RGB")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("shots", type=Path, help="directory of 1x shots")
    ap.add_argument("-o", "--out", type=Path, default=ROOT / "docs/images")
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    for name in SHOTS:
        src = args.shots / f"{name}.png"
        if not src.exists():
            print(f"skip {name} (no {src.name})")
            continue
        img = Image.open(src)
        img = img.convert("RGB") if name in UNSCALED else scaled(img, SCALE)
        rounded(img, RADIUS).save(args.out / f"{name}.png", optimize=True)
        print(f"wrote {name}.png")
    if all((args.shots / f"{n}.png").exists() for n in COVER_SCREENS):
        cover(args.shots).save(args.out / "cover.png", optimize=True)
        print("wrote cover.png")
    if all((args.shots / f"{n}.png").exists() for n in CARDPUTER_SCREENS):
        cover(args.shots, CARDPUTER_SCREENS, "for the M5Stack Cardputer ADV").save(
            args.out / "cover-cardputer.png", optimize=True)
        print("wrote cover-cardputer.png")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
