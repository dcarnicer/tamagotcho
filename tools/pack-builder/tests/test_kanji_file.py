"""`build --kanji-file`: parsing, file order and the CLI wiring."""

import json
from pathlib import Path

import pytest

from tamagotcho_pack import build, cli, paths
from tamagotcho_pack.build import default_deck_name_from_file
from tamagotcho_pack.kanjidic import make_cards, parse_kanji_file, parse_kanjidic

XML = Path(__file__).parent / "data" / "mini_kanjidic2.xml"
NICHI, GETSU, HI = ord("日"), ord("月"), ord("火")


def test_parse_comments_blanks_whitespace_and_bom():
    text = "\ufeff# a textbook chapter\n\n  日  \n\t月\n  # 火 is not in this lesson\n火\n"
    assert parse_kanji_file(text) == ([NICHI, GETSU, HI], [])


def test_parse_removes_duplicates_keeping_first():
    assert parse_kanji_file("月\n火\n月\n火\n")[0] == [GETSU, HI]


def test_parse_names_the_bad_line():
    with pytest.raises(ValueError, match="line 2"):
        parse_kanji_file("日\n日月\n火\n")
    # Blank and comment lines count too, so the error points at the file line.
    with pytest.raises(ValueError, match="line 4"):
        parse_kanji_file("日\n\n  # fine\n火火\n")


def test_parse_reports_unknown_kanji_and_skips_it():
    cps, warnings = parse_kanji_file("日\n亜\n月\n", known={NICHI, GETSU})
    assert cps == [NICHI, GETSU]
    assert len(warnings) == 1
    assert "line 2" in warnings[0] and "亜" in warnings[0] and "KANJIDIC2" in warnings[0]


def _raw_by_codepoint():
    return {k.codepoint: k for k in parse_kanjidic(XML.read_bytes())}


def test_make_cards_file_order_is_the_input_order():
    raw = _raw_by_codepoint()
    cps, _ = parse_kanji_file("月\n火\n日\n")
    cards = make_cards([raw[cp] for cp in cps], "en", order="file")
    assert [c.codepoint for c in cards] == cps
    # `level` is the device's new-card order: 0, 1, 2, ...
    assert [c.level for c in cards] == [0, 1, 2]


def test_make_cards_file_order_can_be_overridden():
    raw = _raw_by_codepoint()
    cps = [GETSU, HI, NICHI]
    cards = make_cards([raw[cp] for cp in cps], "en", order="jlpt")
    # All three are N5 and grade 1, so newspaper frequency decides (日 1, 月 23,
    # 火 574): the requested order is no longer the introduction order.
    assert [c.codepoint for c in cards] == [NICHI, GETSU, HI]


def test_default_deck_name_from_file_is_safe():
    assert default_deck_name_from_file("my-kanji.txt") == "my-kanji"
    assert default_deck_name_from_file("path/to/日本語.txt") == "Kanji"
    assert default_deck_name_from_file("dir/  .txt") == "Kanji"
    assert default_deck_name_from_file("x" * 70 + ".txt") == "x" * 64


class _Stop(ValueError):
    pass


@pytest.fixture
def fake_build(monkeypatch, tmp_path):
    """Points the CLI at the mini KANJIDIC2 and records what it would build."""
    levels = tmp_path / "kanji.json"
    levels.write_text(json.dumps({chr(cp): {"jlpt_new": 5} for cp in (NICHI, GETSU, HI)}),
                      encoding="utf-8")
    monkeypatch.setattr(paths, "KANJIDIC_GZ", XML)
    monkeypatch.setattr(paths, "KANJI_JSON", levels)
    seen = {}

    def fake(cards, font, lang, name, *a, **kw):
        seen.update(cards=[c.codepoint for c in cards], name=name, lang=lang)
        raise _Stop("stop before rendering")

    monkeypatch.setattr(build, "build_pack", fake)
    return seen


def test_cli_kanji_file_selects_in_file_order(fake_build, tmp_path):
    source = tmp_path / "my-kanji.txt"
    source.write_text("# chapter 1\n月\n日\n月\n", encoding="utf-8")
    cli.main(["build", "--kanji-file", str(source), "--lang", "es",
              "-o", str(tmp_path / "out.tgc")])
    assert fake_build["cards"] == [GETSU, NICHI]
    assert fake_build["name"] == "my-kanji"
    assert fake_build["lang"] == "es"


def test_cli_kanji_file_order_can_be_overridden(fake_build, tmp_path):
    source = tmp_path / "my-kanji.txt"
    source.write_text("月\n火\n日\n", encoding="utf-8")
    cli.main(["build", "--kanji-file", str(source), "--order", "jlpt",
              "-o", str(tmp_path / "out.tgc")])
    assert fake_build["cards"] == [NICHI, GETSU, HI]


def test_cli_kanji_file_warns_and_skips_unknown(fake_build, tmp_path, capsys):
    source = tmp_path / "my-kanji.txt"
    source.write_text("日\nA\n月\n", encoding="utf-8")
    cli.main(["build", "--kanji-file", str(source), "-o", str(tmp_path / "out.tgc")])
    assert fake_build["cards"] == [NICHI, GETSU]
    assert "not in KANJIDIC2" in capsys.readouterr().err


def test_cli_file_selection_is_what_the_reorders_see(fake_build, tmp_path, monkeypatch):
    seen = {}

    def fake_common(raw, word_data=None):
        seen["common"] = [k.codepoint for k in raw]
        return raw, None

    def fake_meaning(raw, lang, check_raw=None):
        seen["meaning"] = [k.codepoint for k in raw]
        seen["check"] = [k.codepoint for k in check_raw]
        return raw

    monkeypatch.setattr(cli, "_common_readings", fake_common)
    monkeypatch.setattr(cli, "_meaning_order", fake_meaning)
    source = tmp_path / "my-kanji.txt"
    source.write_text("月\n日\n", encoding="utf-8")
    cli.main(["build", "--kanji-file", str(source), "--common-readings", "--meaning-order",
              "-o", str(tmp_path / "out.tgc")])
    assert seen["common"] == [GETSU, NICHI]
    assert seen["meaning"] == [GETSU, NICHI]
    # Override validation still sees every KANJIDIC2 entry, not only the file.
    assert set(seen["check"]) == {NICHI, GETSU, HI}
    assert fake_build["cards"] == [GETSU, NICHI]


def test_cli_kanji_file_conflicts_with_filters(tmp_path, capsys):
    for extra in (["--jlpt", "5"], ["--grade", "1"], ["--joyo"]):
        rc = cli.main(["build", "--kanji-file", str(tmp_path / "missing.txt"), *extra,
                       "-o", str(tmp_path / "out.tgc")])
        assert rc == 2
        assert "--kanji-file" in capsys.readouterr().err


def test_cli_order_file_needs_kanji_file(tmp_path, capsys):
    rc = cli.main(["build", "--jlpt", "5", "--order", "file",
                   "-o", str(tmp_path / "out.tgc")])
    assert rc == 2
    assert "--order file" in capsys.readouterr().err


def test_cli_kanji_file_conflicts_with_wanikani(tmp_path, capsys):
    rc = cli.main(["build", "--kanji-file", str(tmp_path / "missing.txt"),
                   "--wanikani-token", "token", "-o", str(tmp_path / "out.tgc")])
    assert rc == 2
    assert "--kanji-file" in capsys.readouterr().err
