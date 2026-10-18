"""Tests for `tangocho-pack catalog` (the line-oriented device catalog)."""

import hashlib
import json
from pathlib import Path

from tangocho_pack import catalog, paths
from tangocho_pack.cli import main
from tangocho_pack.glyphs import Glyph
from tangocho_pack.kanjidic import RawKanji, make_cards
from tangocho_pack.pack import read_pack, write_pack

MINI = paths.FIXTURES / "mini.tgc"


def _dir(tmp_path: Path, *names: str) -> Path:
    """A directory holding copies of the committed mini.tgc under *names*."""
    for name in names:
        (tmp_path / name).write_bytes(MINI.read_bytes())
    return tmp_path


def _named_pack(deck_name: str) -> bytes:
    """A one-card pack named *deck_name*, from tiny hand-made glyphs (no font).
    Every non-ASCII or control character of the name gets an empty set-2 glyph,
    so the pack itself is valid and only the catalog title rules apply."""
    nichi = RawKanji(0x65E5, {"en": ["day", "sun"]}, [], [], 4, 1, 4, 1)
    cards = make_cards([nichi], "en", {5}, None, {0x65E5: 5})
    glyphs = [Glyph(0x65E5, 0, 4, 4, 0, -4, 32, b"\x00" * 8),
              Glyph(0x65E5, 1, 4, 4, 0, -4, 16, b"\x00" * 8)]
    extra = sorted({ord(ch) for ch in deck_name if not 0x20 <= ord(ch) <= 0x7E})
    glyphs += [Glyph(cp, 2, 0, 0, 0, 0, 6, b"") for cp in extra]
    raw = write_pack(cards, glyphs, deck_name, "en", 32, 16, 12, 12, 10, build_time=1)
    assert read_pack(raw).errors == []
    return raw


def test_catalog_format_and_sha256(tmp_path):
    d = _dir(tmp_path, "n5-en.tgc")
    records = catalog.scan(d)
    assert [r.name for r in records] == ["n5-en"]
    r = records[0]
    assert r.title == "JLPT N5"  # the pack header's deck_name
    assert r.lang == "en" and r.cards == 3
    assert r.size == MINI.stat().st_size
    assert r.sha256 == hashlib.sha256(MINI.read_bytes()).hexdigest()

    lines = catalog.render(records).splitlines()
    assert lines[0] == "#tangocho-catalog\t2"
    assert lines[1] == "# name\ttitle\tlang\tcards\tbytes\tsha256\turl"
    fields = lines[2].split("\t")
    assert len(fields) == 7
    assert fields[0] == "n5-en" and fields[1] == "JLPT N5" and fields[6] == "n5-en.tgc"
    assert fields[2] == "en" and fields[3] == "3" and fields[4] == str(r.size)
    assert fields[5] == r.sha256
    assert catalog.FILE_NAME == "catalog-v2.txt"


def test_catalog_ordering_is_stable(tmp_path):
    d = _dir(tmp_path, "z.tgc", "a.tgc", "m.tgc")
    assert [r.name for r in catalog.scan(d)] == ["a", "m", "z"]
    assert catalog.scan(d) == catalog.scan(d)
    assert catalog.render(catalog.scan(d)) == catalog.render(catalog.scan(d))


def test_catalog_order_puts_named_packs_first(tmp_path):
    d = _dir(tmp_path, "joyo-es.tgc", "n1-es.tgc", "n5-es.tgc", "n5-en.tgc")
    names = [r.name for r in catalog.scan(d, order=["n5-es", "n1-es", "joyo-es"])]
    assert names == ["n5-es", "n1-es", "joyo-es", "n5-en"]
    try:
        catalog.scan(d, order=["n4-es"])
    except ValueError as e:
        assert "n4-es" in str(e)
    else:
        raise AssertionError("an unknown --order name must fail")
    out = tmp_path / "c.txt"
    assert main(["catalog", str(d), "--order", "n5-en,n5-es", "-o", str(out)]) == 0
    assert [l.split("\t")[0] for l in out.read_text().splitlines()[2:]] == [
        "n5-en", "n5-es", "joyo-es", "n1-es"]


def test_catalog_ignores_non_packs(tmp_path):
    d = _dir(tmp_path, "n5-en.tgc")
    (d / "notes.txt").write_text("not a pack")
    (d / "mini.json").write_text("{}")
    assert [r.name for r in catalog.scan(d)] == ["n5-en"]


def test_catalog_urls(tmp_path):
    d = _dir(tmp_path, "n5-en.tgc")
    # No --base-url: bare file name (relative to the catalog's own URL).
    assert catalog.scan(d)[0].url == "n5-en.tgc"
    # Prefix joined with a single slash, with or without a trailing slash.
    base = "https://example.com/releases/v1.0"
    assert catalog.scan(d, base)[0].url == f"{base}/n5-en.tgc"
    assert catalog.scan(d, base + "/")[0].url == f"{base}/n5-en.tgc"
    # Templated base URL.
    assert catalog.scan(d, "https://cdn.example.com/{file}")[0].url == "https://cdn.example.com/n5-en.tgc"


def test_catalog_cli_writes_text_and_json(tmp_path):
    d = _dir(tmp_path, "n5-en.tgc")
    out, js = tmp_path / "catalog.txt", tmp_path / "catalog.json"
    rc = main(["catalog", str(d), "--base-url", "https://example.com/packs",
               "-o", str(out), "--json", str(js)])
    assert rc == 0
    text = out.read_text(encoding="utf-8")
    assert text.startswith("#tangocho-catalog\t2\n")
    assert "https://example.com/packs/n5-en.tgc" in text
    data = json.loads(js.read_text(encoding="utf-8"))
    assert data["version"] == 2 and data["packs"][0]["name"] == "n5-en"
    assert data["packs"][0]["title"] == "JLPT N5"


def test_catalog_cli_to_stdout(tmp_path, capsys):
    d = _dir(tmp_path, "n5-en.tgc")
    assert main(["catalog", str(d), "-o", "-"]) == 0
    assert capsys.readouterr().out.startswith("#tangocho-catalog\t2\n")


def test_catalog_title_is_the_deck_name(tmp_path):
    # Spaces and non-ASCII UTF-8 are kept as they are; an empty name is allowed
    # (the device then shows the file name).
    (tmp_path / "a.tgc").write_bytes(_named_pack("Joyo kanji"))
    (tmp_path / "b.tgc").write_bytes(_named_pack("Jōyō 漢字"))
    (tmp_path / "c.tgc").write_bytes(_named_pack(""))
    (tmp_path / "d.tgc").write_bytes(_named_pack("x" * catalog.TITLE_MAX_BYTES))
    records = catalog.scan(tmp_path)
    assert [r.title for r in records] == ["Joyo kanji", "Jōyō 漢字", "", "x" * 64]
    lines = catalog.render(records).splitlines()[2:]
    assert [l.split("\t")[1] for l in lines] == ["Joyo kanji", "Jōyō 漢字", "", "x" * 64]
    assert all(len(l.split("\t")) == 7 for l in lines)
    out = tmp_path / "c.txt"
    assert main(["catalog", str(tmp_path), "-o", str(out)]) == 0
    assert "\tJōyō 漢字\ten\t" in out.read_text(encoding="utf-8")


def test_catalog_refuses_bad_titles(tmp_path):
    # Over the byte cap (counted in UTF-8 bytes, not characters), a TAB that
    # would add a field, any other control character: the device would skip
    # the record, so the catalog is not written at all.
    for i, name in enumerate(["x" * 65, "漢" * 22, "JLPT\tN5", "JLPT N5\n", "a\x7fb"]):
        d = tmp_path / str(i)
        d.mkdir()
        (d / "n5-en.tgc").write_bytes(_named_pack(name))
        try:
            catalog.scan(d)
        except ValueError as e:
            assert "n5-en.tgc" in str(e) and "title" in str(e)
        else:
            raise AssertionError(f"deck name {name!r} must be refused")
        out = d / "c.txt"
        assert main(["catalog", str(d), "-o", str(out)]) == 1
        assert not out.exists()
    assert catalog.title_problem("漢" * 21) is None  # 63 bytes


def test_catalog_empty_directory_fails(tmp_path):
    out = tmp_path / "catalog.txt"
    assert main(["catalog", str(tmp_path), "-o", str(out)]) == 1
    assert not out.exists()


def test_catalog_rejects_malformed_pack(tmp_path):
    (tmp_path / "bad.tgc").write_bytes(b"NOTAPACK")
    out = tmp_path / "catalog.txt"
    assert main(["catalog", str(tmp_path), "-o", str(out)]) == 1
    assert not out.exists()


def test_catalog_rejects_structurally_invalid_pack(tmp_path):
    # Correct magic but a broken header, so read_pack() returns errors instead
    # of raising: the command must still refuse it.
    raw = bytearray(MINI.read_bytes())
    raw[8:12] = (0xFFFF).to_bytes(4, "little")  # card_count far past the file
    (tmp_path / "broken.tgc").write_bytes(bytes(raw))
    assert main(["catalog", str(tmp_path), "-o", str(tmp_path / "c.txt")]) == 1


def test_inspect_prints_sha256(capsys):
    assert main(["inspect", str(MINI)]) == 0
    assert hashlib.sha256(MINI.read_bytes()).hexdigest() in capsys.readouterr().out
