"""The Jōyō deck: grade selection, school order (the new-card order on device)
and ASCII deck names."""

import json

import pytest

from tangocho_pack import build, cli, paths
from tangocho_pack.build import default_deck_name
from tangocho_pack.kanjidic import JOYO_GRADES, RawKanji, make_cards, parse_kanjidic


def raw(cp, grade, freq=0, strokes=5, jlpt_old=0):
    return RawKanji(cp, {"en": [f"m{cp:x}"]}, ["オン"], [], jlpt_old, grade, strokes, freq)


# One kanji per interesting case. Codepoints are arbitrary but distinct; the
# expected school order is spelled out in test_grade_order_is_school_order.
RAW = [
    raw(0x4E00, 8, freq=500),               # secondary school, frequent
    raw(0x4E01, 1, freq=900),               # grade 1, rarer
    raw(0x4E02, 9, freq=10),                # jinmeiyō: not Jōyō
    raw(0x4E03, 1, freq=20),                # grade 1, very frequent
    raw(0x4E04, 8, freq=0, strokes=12),     # secondary, no frequency rank
    raw(0x4E05, 6, freq=300),
    raw(0x4E06, 0, freq=1),                 # no grade: not Jōyō
    raw(0x4E07, 8, freq=0, strokes=7),      # secondary, no rank, fewer strokes
    raw(0x4E08, 2, freq=40),
    raw(0x4E09, 10, freq=5),                # jinmeiyō variant: not Jōyō
]
# New-scale JLPT levels for the JLPT-order comparison.
NEW_JLPT = {0x4E00: 3, 0x4E01: 5, 0x4E03: 4, 0x4E05: 2, 0x4E08: 5}
# RAW's Jōyō kanji in school order, and in the (default) JLPT order.
GRADE_ORDER = [0x4E03, 0x4E01, 0x4E08, 0x4E05, 0x4E00, 0x4E07, 0x4E04]
JLPT_ORDER = [0x4E01, 0x4E08, 0x4E03, 0x4E00, 0x4E05, 0x4E04, 0x4E07]


def test_joyo_grades_are_kyoiku_plus_secondary():
    assert JOYO_GRADES == {1, 2, 3, 4, 5, 6, 8}


def test_grade_order_is_school_order():
    cards = make_cards(RAW, "en", None, set(JOYO_GRADES), NEW_JLPT, order="grade")
    # grade 1 by frequency, grade 2, grade 6, then grade 8: ranked kanji first,
    # unranked ones last with fewer strokes first. Grades 0, 9 and 10 are out.
    assert [c.codepoint for c in cards] == GRADE_ORDER
    # `level` (the device's new-card order) follows the list.
    assert [c.level for c in cards] == list(range(len(cards)))


def test_jlpt_order_is_unchanged_default():
    cards = make_cards(RAW, "en", None, set(JOYO_GRADES), NEW_JLPT)
    # N5 first (grade, then frequency inside a level), then N4, N3, N2, then the
    # kanji with no JLPT level (no stroke tie-break in this order).
    assert [c.codepoint for c in cards] == JLPT_ORDER


def test_unknown_order_is_refused():
    with pytest.raises(ValueError, match="order"):
        make_cards(RAW, "en", None, set(JOYO_GRADES), NEW_JLPT, order="random")


def test_default_deck_names_are_ascii():
    assert default_deck_name(set(), set(JOYO_GRADES), joyo=True) == "Joyo kanji"
    assert default_deck_name({1}, set(JOYO_GRADES), joyo=True) == "JLPT N1 Joyo"
    # The existing names are unchanged.
    assert default_deck_name({5}, set()) == "JLPT N5"
    assert default_deck_name({5, 4}, set()) == "JLPT N5+N4"
    assert default_deck_name(set(), {1, 2}) == "Grade 1+2"
    for n in range(1, 6):
        assert default_deck_name({n}, set()).isascii()
    assert default_deck_name(set(), set(), joyo=True).isascii()


# --- CLI ---------------------------------------------------------------------

def _xml(entries: list[RawKanji]) -> str:
    chars = []
    for k in entries:
        misc = f"<stroke_count>{k.strokes}</stroke_count>"
        if k.grade:
            misc = f"<grade>{k.grade}</grade>" + misc
        if k.freq:
            misc += f"<freq>{k.freq}</freq>"
        chars.append(
            f"<character><literal>{chr(k.codepoint)}</literal>"
            f'<codepoint><cp_value cp_type="ucs">{k.codepoint:x}</cp_value></codepoint>'
            f"<misc>{misc}</misc>"
            '<reading_meaning><rmgroup><reading r_type="ja_on">オン</reading>'
            f"<meaning>m{k.codepoint:x}</meaning></rmgroup></reading_meaning></character>")
    return ('<?xml version="1.0" encoding="UTF-8"?><kanjidic2><header>'
            "<file_version>4</file_version></header>" + "".join(chars) + "</kanjidic2>")


class _Stop(ValueError):
    pass


@pytest.fixture
def fake_build(monkeypatch, tmp_path):
    """Points the CLI at a synthetic KANJIDIC2 and records what it would build."""
    xml = tmp_path / "kanjidic2.xml"
    xml.write_text(_xml(RAW), encoding="utf-8")
    levels = tmp_path / "kanji.json"
    levels.write_text(json.dumps({chr(cp): {"jlpt_new": n} for cp, n in NEW_JLPT.items()}),
                      encoding="utf-8")
    monkeypatch.setattr(paths, "KANJIDIC_GZ", xml)
    monkeypatch.setattr(paths, "KANJI_JSON", levels)
    seen = {}

    def fake(cards, font, lang, name, *a, **kw):
        seen.update(cards=[c.codepoint for c in cards], name=name, lang=lang, strokes=kw.get("strokes"))
        raise _Stop("stop before rendering")

    monkeypatch.setattr(build, "build_pack", fake)
    return seen


def test_cli_joyo_selects_grades_in_school_order(fake_build, tmp_path, capsys):
    rc = cli.main(["build", "--joyo", "--lang", "es", "--strokes", "-o", str(tmp_path / "j.tgc")])
    assert rc == 2  # the fake build_pack stops before rendering
    assert fake_build["name"] == "Joyo kanji"
    assert fake_build["lang"] == "es" and fake_build["strokes"] is True
    assert fake_build["cards"] == GRADE_ORDER
    assert "not ASCII" not in capsys.readouterr().err


def test_cli_joyo_order_can_be_overridden(fake_build, tmp_path):
    cli.main(["build", "--joyo", "--order", "jlpt", "-o", str(tmp_path / "j.tgc")])
    assert fake_build["cards"] == JLPT_ORDER
    assert fake_build["name"] == "Joyo kanji"


def test_cli_joyo_and_grade_conflict(fake_build, tmp_path, capsys):
    rc = cli.main(["build", "--joyo", "--grade", "1", "-o", str(tmp_path / "j.tgc")])
    assert rc == 2
    assert "--joyo" in capsys.readouterr().err
    assert "cards" not in fake_build


def test_cli_warns_about_a_non_ascii_name(fake_build, tmp_path, capsys):
    cli.main(["build", "--joyo", "--name", "Jōyō", "-o", str(tmp_path / "j.tgc")])
    assert fake_build["name"] == "Jōyō"
    assert "not ASCII" in capsys.readouterr().err


@pytest.mark.skipif(not paths.KANJIDIC_GZ.exists(), reason="run `tangocho-pack fetch`")
def test_real_kanjidic_has_the_2136_joyo_kanji():
    cards = make_cards(parse_kanjidic(paths.KANJIDIC_GZ), "en", None, set(JOYO_GRADES),
                       {}, order="grade")
    assert len(cards) == 2136
    assert [c.grade for c in cards] == sorted(c.grade for c in cards)
    assert cards[0].grade == 1 and cards[-1].grade == 8
