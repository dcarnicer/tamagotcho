#!/usr/bin/env python3
"""Render the Tamagotcho ESP Web Tools install page.

The template is tools/install-page/index.html: it contains the {{TAG}} and
{{VERSION}} placeholders and references the release manifests with relative
paths. The rendered page must be served from the same directory as the
manifests and merged images (GitHub Pages does that), because ESP Web Tools
cannot fetch GitHub release assets: they send no CORS headers.
"""

from __future__ import annotations

import argparse
import os
import re
import sys
import tempfile
from html.parser import HTMLParser
from pathlib import Path

HERE = Path(__file__).resolve().parent
TEMPLATE = HERE / "index.html"
TOKEN_RE = re.compile(r"\{\{[^{}\n]+\}\}")


class References(HTMLParser):
    """Collect the assets a rendered page needs."""

    def __init__(self) -> None:
        super().__init__(convert_charrefs=True)
        self.manifests: list[str] = []
        self.assets: list[str] = []

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        values = dict(attrs)
        if tag == "esp-web-install-button" and values.get("manifest"):
            self.manifests.append(values["manifest"])
        if tag == "a" and "data-asset" in values and values.get("href"):
            self.assets.append(values["href"])


def parse_references(html: str) -> References:
    parser = References()
    parser.feed(html)
    parser.close()
    return parser


def resolve_out(raw: str) -> Path:
    """Accept an .html file or a directory (e.g. --out site)."""
    path = Path(raw).expanduser()
    directory_hint = raw.endswith(("/", os.sep)) or path.is_dir() or not path.suffix
    return path / "index.html" if directory_hint else path


def local_reference(reference: str) -> str | None:
    """Return a same-origin relative path, or None for external/anchor links."""
    if reference.startswith(("#", "//")) or re.match(r"^[a-zA-Z][a-zA-Z0-9+.-]*:", reference):
        return None
    path = reference.split("?", 1)[0].split("#", 1)[0]
    return path or None


def missing_references(references: list[str], assets: Path) -> list[str]:
    missing = []
    seen = set()
    for reference in references:
        path = local_reference(reference)
        if path is None or reference in seen:
            continue
        seen.add(reference)
        if not (assets / path).is_file():
            missing.append(reference)
    return missing


def warn_missing(manifests: list[str], assets: Path) -> None:
    missing = missing_references(manifests, assets)
    if missing:
        print(
            "warning: manifests missing from "
            f"{assets}: {', '.join(missing)} (download them from the release)",
            file=sys.stderr,
        )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Render the Tamagotcho browser install page.",
        epilog=(
            "Example: python3 tools/install-page/render.py --tag v0.4.2 "
            "--out site/index.html --assets site"
        ),
    )
    parser.add_argument("--tag", required=True, help="release tag, e.g. v0.4.2")
    parser.add_argument(
        "--version",
        help="version shown on the page (default: the tag without a leading 'v')",
    )
    parser.add_argument(
        "--out",
        help="output .html file or directory (e.g. site); required unless --check",
    )
    parser.add_argument(
        "--assets",
        type=Path,
        help="directory with the release manifests and images (checks they exist)",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help=(
            "render to a temporary directory and fail if a {{TOKEN}} is left or a "
            "referenced manifest/fallback file is missing"
        ),
    )
    args = parser.parse_args()

    tag = args.tag.strip()
    if not tag:
        parser.error("--tag must not be empty")
    version = args.version.strip() if args.version else (tag[1:] if tag.startswith("v") else tag)

    try:
        template = TEMPLATE.read_text(encoding="utf-8")
    except OSError as error:
        print(f"error: cannot read {TEMPLATE}: {error}", file=sys.stderr)
        return 1

    html = template.replace("{{TAG}}", tag).replace("{{VERSION}}", version)
    refs = parse_references(html)
    if not refs.manifests:
        print("error: the rendered page has no esp-web-install-button", file=sys.stderr)
        return 1

    if args.check:
        assets = args.assets
        if assets is None and args.out:
            assets = resolve_out(args.out).parent
        if assets is None:
            parser.error("--check needs --assets or --out to locate the release files")
        assets = assets.expanduser()

        with tempfile.TemporaryDirectory(prefix="tamagotcho-install-") as temporary:
            rendered = Path(temporary) / "index.html"
            rendered.write_text(html, encoding="utf-8")
            errors: list[str] = []

            leftovers = sorted(set(TOKEN_RE.findall(html)))
            if leftovers:
                errors.append(f"unreplaced tokens: {', '.join(leftovers)}")
            if not assets.is_dir():
                errors.append(f"assets directory does not exist: {assets}")
            else:
                missing = missing_references(refs.manifests + refs.assets, assets)
                if missing:
                    errors.append(
                        f"files referenced by the page are missing from {assets}: "
                        + ", ".join(missing)
                    )

            if errors:
                for error in errors:
                    print(f"error: {error}", file=sys.stderr)
                return 1
            print(
                f"check OK for {tag}: no unreplaced tokens, "
                f"{len(refs.manifests)} install buttons and "
                f"{len(refs.assets)} download links resolve"
            )
        return 0

    if not args.out:
        parser.error("--out is required unless --check is used")

    out = resolve_out(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(html, encoding="utf-8")
    print(f"wrote {out} for {tag} (version {version})")

    if args.assets:
        warn_missing(refs.manifests, args.assets.expanduser())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
