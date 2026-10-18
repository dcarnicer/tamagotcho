"""Tests for `tangocho-pack catalog` (the line-oriented device catalog)."""

import hashlib
import json
from pathlib import Path

from tangocho_pack import catalog, paths
from tangocho_pack.cli import main

MINI = paths.FIXTURES / "mini.tgc"


def _dir(tmp_path: Path, *names: str) -> Path:
    """A directory holding copies of the committed mini.tgc under *names*."""
    for name in names:
        (tmp_path / name).write_bytes(MINI.read_bytes())
    return tmp_path


def test_catalog_format_and_sha256(tmp_path):
    d = _dir(tmp_path, "n5-en.tgc")
    records = catalog.scan(d)
    assert [r.name for r in records] == ["n5-en"]
    r = records[0]
    assert r.lang == "en" and r.cards == 3
    assert r.size == MINI.stat().st_size
    assert r.sha256 == hashlib.sha256(MINI.read_bytes()).hexdigest()

    lines = catalog.render(records).splitlines()
    assert lines[0] == "#tangocho-catalog\t1"
    assert lines[1].startswith("# name\t")
    fields = lines[2].split("\t")
    assert len(fields) == 6
    assert fields[0] == "n5-en" and fields[5] == "n5-en.tgc"
    assert fields[2] == "3" and fields[3] == str(r.size)
    assert fields[4] == r.sha256


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
    assert text.startswith("#tangocho-catalog\t1\n")
    assert "https://example.com/packs/n5-en.tgc" in text
    data = json.loads(js.read_text(encoding="utf-8"))
    assert data["version"] == 1 and data["packs"][0]["name"] == "n5-en"


def test_catalog_cli_to_stdout(tmp_path, capsys):
    d = _dir(tmp_path, "n5-en.tgc")
    assert main(["catalog", str(d), "-o", "-"]) == 0
    assert capsys.readouterr().out.startswith("#tangocho-catalog\t1\n")


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
