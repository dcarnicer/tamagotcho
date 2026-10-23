"""Example words (`build --example-words`, pack format v3, `sync-glosses`).

The word data are the hand-written miniatures in tests/data/ (mini_jmdict.xml,
mini_furigana.txt, mini_glosses_es.tsv) and a few inline ones, in the sources'
formats with glosses written for the tests; not copied from JMdict, FreeDict
or the project's table."""

import io
import json
import sqlite3
import struct
import tarfile

import pytest

from tamagotcho_pack import build, cli, fixture, joyo_table, paths
from tamagotcho_pack.kanjidic import make_cards, parse_kanjidic
from tamagotcho_pack.pack import (HEADER_SIZE, HEADER_SIZE_V3, WORD_ENTRY_SIZE, read_pack,
                                  write_pack)
from tamagotcho_pack.readings import parse_furigana, weighted_pairs
from tamagotcho_pack.words import (KIND_KUN, KIND_ON, ExampleWord, GlossRow, Info, Word,
                                   append_rows, english_gloss, example_words, is_transliteration,
                                   load_gloss_table, load_word_info, pick, pick_words,
                                   reading_span, shown_gloss, skip_words, translate,
                                   word_candidates)

needs_font = pytest.mark.skipif(not paths.DEFAULT_FONT.exists(), reason="run `tamagotcho-pack fetch`")


def mini_cards(lang="es"):
    return make_cards(parse_kanjidic(fixture.MINI_XML.read_bytes()), lang, {5}, None,
                      fixture.MINI_JLPT)


def picked(picks):
    return [[(p.kind, p.asked, p.word.text if p.word else None) for p in ps] for ps in picks]


# --- the pieces ---------------------------------------------------------------------

@pytest.mark.parametrize("text, reading, segs, index, span", [
    ("毎日", "まいにち", [(0, 0, "まい"), (1, 1, "にち")], 1, (6, 6)),
    ("学校", "がっこう", [(0, 0, "がっ"), (1, 1, "こう")], 0, (0, 6)),
    ("食べる", "たべる", [(0, 0, "た")], 0, (0, 3)),
    ("お金", "おかね", [(1, 1, "かね")], 0, (3, 6)),
    ("火曜日", "かようび", [(0, 0, "か"), (1, 1, "よう"), (2, 2, "び")], 2, (9, 3)),
    ("ガス代", "ガスだい", [(2, 2, "だい")], 0, (6, 6)),     # katakana in the word
    ("大人", "おとな", [(0, 1, "おとな")], 0, (0, 9)),        # one part for two kanji
    ("毎日", "まいあさ", [(0, 0, "まい"), (1, 1, "にち")], 1, (0, 0)),  # does not spell it
    ("毎日", "まいにち", [(0, 0, "まい")], 1, (0, 0)),          # no such segment
])
def test_reading_span(text, reading, segs, index, span):
    assert reading_span(text, reading, segs, index) == span


def test_transliterations_and_shown_gloss():
    assert is_transliteration("sake", "さけ")
    assert is_transliteration('"oden": a stew', "おでん")
    assert is_transliteration("Tokyo", "とうきょう") is False   # a proper name translates
    assert shown_gloss(["kimono", "Japanese clothing"], "きもの") == "Japanese clothing"
    assert shown_gloss(["neuron (eng: neuron)"], "ニューロン") == "neuron"


def test_english_gloss_short_rule():
    info = Info(1.0, True, False, eng=["to come (spatially or temporally)", "to approach"])
    assert english_gloss(info, "くる") == "to come"             # too long: notes dropped
    assert english_gloss(Info(1.0, True, False, eng=["yen (currency of Japan)"]), "えん") \
        == "yen (currency of Japan)"                            # short: notes kept
    info = Info(1.0, True, False, eng=["a very long gloss without any notes at all"])
    assert english_gloss(info, "x") == ""                       # still too long: none
    assert english_gloss(Info(1.0, True, False, eng=["sake", "rice wine"]), "さけ") == "rice wine"
    assert english_gloss(Info(1.0, True, False, eng=["x" * 24]), "x") == "x" * 24
    assert english_gloss(None, "x") == ""


def test_load_word_info():
    info = load_word_info(fixture.MINI_JMDICT)
    assert info[("毎日", "まいにち")].eng == ["every day", "daily"]
    assert info[("毎日", "まいにち")].tagged and not info[("毎日", "まいにち")].uk
    assert info[("毎日", "まいにち")].pos == ("adv", "n")
    assert not info[("日", "にち")].tagged                     # no priority tag
    assert info[("火事", "かじ")].uk                            # usually kana
    assert info[("火事", "かじ")].other == []


def test_load_word_info_other_language_and_restrictions():
    xml = """<?xml version="1.0"?><!DOCTYPE JMdict [<!ENTITY n "noun">]><JMdict>
<entry><k_ele><keb>上手</keb><ke_pri>ichi1</ke_pri></k_ele><k_ele><keb>上手い</keb></k_ele>
<r_ele><reb>じょうず</reb><re_restr>上手</re_restr><re_pri>ichi1</re_pri></r_ele>
<r_ele><reb>うまい</reb></r_ele>
<sense><stagr>うまい</stagr><gloss>tasty</gloss></sense>
<sense><gloss>skilful</gloss><gloss xml:lang="spa">hábil</gloss></sense>
</entry></JMdict>""".encode()
    info = load_word_info(xml, other_lang="spa")
    assert ("上手", "うまい") in info and ("上手い", "じょうず") not in info
    assert info[("上手", "じょうず")].eng == ["skilful"]      # the うまい-only sense is skipped
    assert info[("上手", "うまい")].eng == ["tasty"]
    assert info[("上手", "じょうず")].other == ["hábil"]
    assert load_word_info(xml)[("上手", "じょうず")].other == []


def test_pick_order_filters_and_skip():
    w = {(t, t.lower()): Word(t, t.lower(), wt, lv) for t, wt, lv in
         (("A", 9.0, 0), ("B", 1.0, 5), ("C", 5.0, 3), ("D", 99.0, 0), ("E", 50.0, 0))}
    # A is untagged and on no list; D is usually written in kana; the rest are tagged.
    info = {k: Info(1.0, k[0] != "A", k[0] == "D") for k in w}
    # A JLPT list word at or below the kanji's level first, then the weight.
    assert pick(w, 5).text == "B"                       # C (N3) is above an N5 kanji's level
    assert pick(w, 1).text == "C"                       # any list word: C is heavier
    assert pick(w, 5, info).text == "B"
    gone = {("B", "b"), ("C", "c")}
    assert pick(w, 5, skip=gone).text == "D"            # no list word left: the weight
    assert pick(w, 5, info, skip=gone).text == "E"      # D usually kana, A untagged
    assert pick(w, 5, info, skip=gone | {("E", "e")}) is None
    assert pick({}, 5) is None and pick(None, 5) is None


def test_mini_picks():
    cards = mini_cards()
    info = load_word_info(fixture.MINI_JMDICT)
    cands = word_candidates(cards, weighted_pairs(fixture.MINI_JMDICT, jlpt_vocab=fixture.MINI_VOCAB),
                            parse_furigana(fixture.MINI_FURIGANA), fixture.MINI_VOCAB)
    assert picked(pick_words(cards, cands, info)) == [
        [(KIND_ON, "ニチ", "毎日"), (KIND_KUN, "ひ", "日")],         # 日 にち: untagged
        [(KIND_ON, "ゲツ", "今月"), (KIND_KUN, "つき", "月")],
        [(KIND_ON, "カ", "火曜日"), (KIND_KUN, "ひ", "火")],         # the list word first
    ]
    skip = skip_words(load_gloss_table(fixture.MINI_GLOSSES_ES))
    assert skip == {("火曜日", "かようび")}
    assert picked(pick_words(cards, cands, info, skip))[2][0] == (KIND_ON, "カ", "火山")  # 火事: uk
    # A card whose kind is empty asks nothing of it, and gets no word.
    cards[1].kunyomi = []
    assert picked(pick_words(cards, cands, info))[1] == [(KIND_ON, "ゲツ", "今月")]
    # A dropped kind's readings still claim their words (Card.valid_*).
    cards[0].onyomi, cards[0].valid_onyomi = ["ジツ"], ["ジツ", "ニチ"]
    assert picked(pick_words(cards, cands, info))[0][0] == (KIND_ON, "ジツ", None)


def test_example_words_glosses_by_language():
    cards = mini_cards()
    info = load_word_info(fixture.MINI_JMDICT)
    table = load_gloss_table(fixture.MINI_GLOSSES_ES)
    cands = word_candidates(cards, weighted_pairs(fixture.MINI_JMDICT, jlpt_vocab=fixture.MINI_VOCAB),
                            parse_furigana(fixture.MINI_FURIGANA), fixture.MINI_VOCAB)
    picks = pick_words(cards, cands, info, skip_words(table))
    gl = {lang: [[w.gloss for w in ws] for ws in example_words(picks, info, lang, table)]
          for lang in ("en", "es", "fr")}
    assert gl["en"] == [["every day", "day"], ["this month", "moon"], ["volcano", "fire"]]
    # Spanish: ok rows only (今月 is a draft, 月 つき has no row).
    assert gl["es"] == [["todos los días", "día"], ["", ""], ["volcán", "fuego"]]
    assert gl["fr"] == [["", ""], ["", ""], ["", ""]]
    ws = example_words(picks, info, "es", table)
    assert (ws[0][0].text, ws[0][0].reading, ws[0][0].span) == ("毎日", "まいにち", (6, 6))


# --- the Spanish table ------------------------------------------------------------------

HEADER = "level\tkanji\tword\treading\ten\tes\tstatus\tnote\n"


def test_gloss_table_parse(tmp_path):
    t = tmp_path / "t.tsv"
    # Columns may come in any order; a missing trailing note is fine.
    t.write_text("# comment\n\nstatus\tword\treading\tes\tlevel\tkanji\ten\tnote\n"
                 "ok\t毎日\tまいにち\ttodos los días\tN5\t日,毎\tevery day\n"
                 "skip\t本木\tもとき\t\tN5\t本\toriginal stock\trare\n", encoding="utf-8")
    rows = load_gloss_table(t)
    assert rows[("毎日", "まいにち")].es == "todos los días"
    assert rows[("毎日", "まいにち")].kanji == "日,毎" and rows[("毎日", "まいにち")].note == ""
    assert skip_words(rows) == {("本木", "もとき")}
    assert load_gloss_table(tmp_path / "missing.tsv") == {}


@pytest.mark.parametrize("body, msg", [
    ("N5\t日\t日\tひ\tday\tdía\tok\t\nN5\t日\t日\tひ\tday\tdía\tdraft\t\n", "already on line"),
    ("N5\t日\t日\tひ\tday\tdía\tdone\t\n", "status"),
    ("N5\t日\t日\tひ\tday\t\tok\t\n", "needs a gloss"),
    ("N5\t日\t日\n", "columns"),
    ("N5\t日\t\tひ\tday\tdía\tok\t\n", "empty word"),
])
def test_gloss_table_errors(tmp_path, body, msg):
    t = tmp_path / "t.tsv"
    t.write_text(HEADER + body, encoding="utf-8")
    with pytest.raises(ValueError, match=msg):
        load_gloss_table(t)


def test_gloss_table_header_required(tmp_path):
    t = tmp_path / "t.tsv"
    t.write_text("level\tword\treading\n", encoding="utf-8")
    with pytest.raises(ValueError, match="header lacks"):
        load_gloss_table(t)


def test_append_rows_keeps_every_byte(tmp_path):
    t = tmp_path / "t.tsv"
    old = "# keep  me \n" + HEADER + "N5\t日\t日\tひ\tday\tdía\tok\tno newline at the end"
    t.write_bytes(old.encode())
    append_rows(t, [GlossRow("N5", "月", "月", "つき", "moon", "luna", "draft", "CHECK: x")])
    data = t.read_bytes().decode()
    assert data.startswith(old + "\n")
    assert data.endswith("N5\t月\t月\tつき\tmoon\tluna\tdraft\tCHECK: x\n")
    assert len(load_gloss_table(t)) == 2
    # A missing table is created with a header.
    n = tmp_path / "new" / "t.tsv"
    append_rows(n, [GlossRow("N5", "月", "月", "つき", "moon", "", "draft", "")])
    assert list(load_gloss_table(n)) == [("月", "つき")]
    with pytest.raises(ValueError):
        append_rows(n, [GlossRow("N5", "月", "a\tb", "x", "", "", "draft", "")])


def test_translate_first_english_gloss_freedict_has():
    from tamagotcho_pack.words import FdEntry
    fd = {"rest": [FdEntry("n", [["descanso"]]), FdEntry("v", [["descansar", "reposar"]])],
          "holiday": [FdEntry("n", [["vacaciones"], ["feriado"]])]}
    assert translate(["to rest", "to be absent"], ("v5m",), fd) == ("descansar", "to rest")
    assert translate(["day off", "holiday"], ("n",), fd) == ("vacaciones", "holiday")
    assert translate(["nothing here"], ("n",), fd) is None


# --- pack format v3 ---------------------------------------------------------------------

def _words_pack(words, cards=None, **kw):
    """A v3 pack from the mini cards with hand-made glyphs (no font needed)."""
    from tamagotcho_pack.glyphs import Glyph
    from tamagotcho_pack.pack import card_strings
    cards = cards or mini_cards("en")
    text = set(range(0x20, 0x7F))
    for c in cards:
        text |= {ord(ch) for s in card_strings(c) for ch in s}
    for ws in words or []:
        for w in ws:
            text |= {ord(ch) for ch in w.text + w.reading + w.gloss}
    glyphs = [Glyph(c.codepoint, s, 2, 2, 0, -2, 3, b"\xff\xff") for c in cards for s in (0, 1)]
    glyphs += [Glyph(cp, 2, 1, 1, 0, -1, 2, b"\xf0") for cp in sorted(text)]
    from tamagotcho_pack.pack import on_hiragana
    glyphs += [Glyph(ord(ch), 2, 1, 1, 0, -1, 2, b"\xf0") for c in cards
               for ch in set(on_hiragana("".join(c.onyomi))) - {chr(cp) for cp in text}]
    return write_pack(cards, glyphs, "Mini", "en", 32, 16, 12, 14, 11, build_time=0,
                      words=words, **kw)


def mini_example_words():
    return [[ExampleWord(KIND_ON, "毎日", "まいにち", "every day", (6, 6)),
             ExampleWord(KIND_KUN, "日", "ひ", "day", (0, 3))],
            [],
            [ExampleWord(KIND_KUN, "火", "ひ", "", (0, 3))]]


def test_v3_roundtrip_and_layout():
    raw = _words_pack(mini_example_words())
    p = read_pack(raw)
    assert p.errors == []
    h = p.header
    assert (h["version"], h["header_size"], h["cards_off"]) == (3, HEADER_SIZE_V3, HEADER_SIZE_V3)
    wo = h["words_off"]
    assert wo % 4 == 0 and wo >= h["glyph_data_off"]
    assert struct.unpack_from("<HHI", raw, wo) == (WORD_ENTRY_SIZE, 0, 3)
    assert struct.unpack_from("<4I", raw, wo + 8) == (0, 2, 2, 3)
    assert len(raw) == wo + 8 + 16 + 3 * WORD_ENTRY_SIZE
    assert [[(w.kind, w.word, w.reading, w.gloss, w.reading_start, w.reading_len) for w in ws]
            for ws in p.words] == [[(0, "毎日", "まいにち", "every day", 6, 6),
                                    (1, "日", "ひ", "day", 0, 3)], [], [(1, "火", "ひ", "", 0, 3)]]
    # An empty gloss is an empty string inside the pool, never offset 0.
    assert p.words[2][0].string_offs[2] >= h["strings_off"]
    # The extension's reserved words are zero.
    assert struct.unpack_from("<4I", raw, HEADER_SIZE) == (wo, 0, 0, 0)


def test_v3_without_words_and_v2_unchanged():
    v2 = _words_pack(None)
    assert read_pack(v2).header["version"] == 2 and read_pack(v2).header["header_size"] == 64
    assert read_pack(v2).words == [[], [], []]
    empty = read_pack(_words_pack([[], [], []]))
    assert empty.errors == [] and empty.header["version"] == 3
    assert struct.unpack_from("<HHI", empty.raw, empty.header["words_off"])[2] == 0


def test_writer_rejects_bad_words():
    with pytest.raises(ValueError, match="aligned"):
        _words_pack([[]])
    with pytest.raises(ValueError, match="one per kind"):
        _words_pack([[ExampleWord(KIND_KUN, "日", "ひ"), ExampleWord(KIND_ON, "毎日", "まいにち")],
                     [], []])
    with pytest.raises(ValueError, match="span"):
        _words_pack([[ExampleWord(KIND_ON, "毎日", "まいにち", "", (3, 0))], [], []])


def _corrupt(raw: bytes, off: int, fmt: str, *vals) -> list[str]:
    bad = bytearray(raw)
    struct.pack_into(fmt, bad, off, *vals)
    return read_pack(bytes(bad)).errors


def test_v3_validation():
    raw = _words_pack(mini_example_words())
    h = read_pack(raw).header
    wo = h["words_off"]
    table = wo + 8 + 4 * 4
    assert any("header_size" in e for e in _corrupt(raw, 6, "<H", 64))
    assert any("reserved" in e for e in _corrupt(raw, 68, "<I", 1))
    assert any("aligned" in e for e in _corrupt(raw, 64, "<I", wo + 2))
    assert any("entry_size" in e for e in _corrupt(raw, wo, "<H", 12))
    assert any("index" in e for e in _corrupt(raw, wo + 8 + 4, "<I", 3))       # 0,3,2,3
    assert any("index" in e for e in _corrupt(raw, wo + 4, "<I", 4))           # count != last
    assert any("bad kind" in e for e in _corrupt(raw, table + 12, "<B", 2))
    assert any("span" in e for e in _corrupt(raw, table + 14, "<B", 7))        # mid-character
    assert any("span" in e for e in _corrupt(raw, table + 14, "<BB", 9, 6))    # past the end
    assert any("outside string pool" in e for e in _corrupt(raw, table + 8, "<I", 0))
    assert any("kun'yomi before" in e
               for e in _corrupt(raw, table + 12, "<B", 1))                    # two kun words
    # A word that does not contain its card's kanji.
    other = read_pack(raw).words[2][0].string_offs[0]
    assert any("does not contain" in e for e in _corrupt(raw, table, "<I", other))


def test_v3_needs_set2_glyphs_for_words():
    raw = _words_pack(mini_example_words())
    p = read_pack(raw)
    g = p.find_glyph(2, ord("毎"))
    assert g is not None
    # Make 毎's set-2 entry another code point (keeping the table sorted).
    idx = p.glyphs.index(g)
    bad = bytearray(raw)
    struct.pack_into("<I", bad, p.header["glyphs_off"] + idx * 16, ord("毎") - 1)
    assert any("missing from set 2" in e and "U+6BCE" in e for e in read_pack(bytes(bad)).errors)


@needs_font
def test_build_pack_words_and_glyphs():
    cards = mini_cards("en")
    raw = build.build_pack(cards, paths.DEFAULT_FONT, "en", "JLPT N5", 32, 16, 12, 18,
                           build_time=0, words=fixture.mini_words(cards, "en"))
    p = read_pack(raw)
    assert p.errors == [] and p.header["version"] == 3
    set2 = {chr(g.codepoint) for g in p.glyphs if g.set == 2}
    assert set("毎日今月火山まいにちこんげつかざん") <= set2
    assert not {chr(g.codepoint) for g in p.glyphs if g.set == 3} & set("毎今山")


@needs_font
def test_build_pack_without_words_is_unchanged():
    cards = mini_cards("en")
    # The v2 baseline (tests/data/mini_v2_baseline.tgc) is the same build.
    a = fixture.build_mini(paths.DEFAULT_FONT, strokes=False)
    assert read_pack(a).header["version"] == 2
    assert a == (paths.ROOT / "tests" / "data" / "mini_v2_baseline.tgc").read_bytes()
    assert a == build.build_pack(cards, paths.DEFAULT_FONT, "en", "JLPT N5", build_time=0,
                                 words=None, **fixture.SIZES)


@needs_font
def test_build_pack_drops_words_the_font_cannot_draw(capsys):
    cards = mini_cards("en")
    words = [[ExampleWord(KIND_ON, "毎日", "まいにち", "every day \U000F0000", (6, 6)),
              ExampleWord(KIND_KUN, "日\U000F0001", "ひ", "day", (0, 3))], [], []]
    p = read_pack(build.build_pack(cards, paths.DEFAULT_FONT, "en", "N5", 32, 16, 12, 18,
                                   build_time=0, words=words))
    assert p.errors == []
    assert [(w.word, w.gloss) for w in p.words[0]] == [("毎日", "")]
    err = capsys.readouterr().err
    assert "left out" in err and "without one" in err


@needs_font
def test_mini_words_fixture_is_committed_and_deterministic():
    a = fixture.build_mini_words(paths.DEFAULT_FONT)
    assert a == fixture.build_mini_words(paths.DEFAULT_FONT)
    committed = paths.FIXTURES / "mini_words.tgc"
    assert a == committed.read_bytes(), "run `tamagotcho-pack fixture`"
    expected = json.loads((paths.FIXTURES / "mini_words.json").read_text(encoding="utf-8"))
    assert fixture.words_json(a) == expected
    p = read_pack(a)
    assert [[w.word for w in ws] for ws in p.words] == [["毎日", "日"], ["今月", "月"],
                                                         ["火山", "火"]]
    assert [[w.gloss for w in ws] for ws in p.words] == [["todos los días", "día"], ["", ""],
                                                          ["volcán", "fuego"]]


# --- the command line ---------------------------------------------------------------------

def _msgpack(v) -> bytes:
    if isinstance(v, list):
        return bytes([0x90 | len(v)]) + b"".join(_msgpack(x) for x in v)
    if isinstance(v, dict):
        return bytes([0x80 | len(v)]) + b"".join(_msgpack(k) + _msgpack(x) for k, x in v.items())
    b = v.encode("utf-8")
    return bytes([0xD9, len(b)]) + b


def _anki_deck(path, rows):
    con = sqlite3.connect(path)
    con.execute("create table fieldModels (id integer, name text, ordinal integer)")
    con.execute("create table fields (factId integer, fieldModelId integer, ordinal integer, "
                "value text)")
    con.executemany("insert into fieldModels values (?, ?, ?)", [(7, "Front", 0), (8, "Back", 1)])
    for i, (front, back) in enumerate(rows):
        con.execute("insert into fields values (?, 7, 0, ?)", (i, front))
        con.execute("insert into fields values (?, 8, 1, ?)", (i, back))
    con.commit()
    con.close()
    return path


def _freedict(path):
    entries = [("every day", "n", "cada día"), ("this month", "n", "este mes"),
               ("moon", "n", "luna"), ("Tuesday", "n", "martes"), ("volcano", "n", "volcán")]
    body = "".join(f'<entry><form><orth>{en}</orth></form><gramGrp><pos>{pos}</pos></gramGrp>'
                   f'<sense><cit type="trans" xml:lang="es"><quote>{es}</quote></cit></sense>'
                   f'</entry>' for en, pos, es in entries)
    tei = ('<?xml version="1.0" encoding="UTF-8"?><TEI xmlns="http://www.tei-c.org/ns/1.0">'
           f'<teiHeader/><text><body>{body}</body></text></TEI>').encode()
    with tarfile.open(path, "w:xz") as t:
        info = tarfile.TarInfo("eng-spa/eng-spa.tei")
        info.size = len(tei)
        t.addfile(info, io.BytesIO(tei))
    return path


class _Stop(Exception):
    pass


@pytest.fixture
def word_inputs(monkeypatch, tmp_path):
    levels = tmp_path / "kanji.json"
    levels.write_text(json.dumps({"日": {"jlpt_new": 5}, "月": {"jlpt_new": 5},
                                  "火": {"jlpt_new": 5}}), encoding="utf-8")
    files = {"KANJIDIC_GZ": fixture.MINI_XML, "KANJI_JSON": levels,
             "JMDICT_GZ": fixture.MINI_JMDICT, "JMDICT_FURIGANA": fixture.MINI_FURIGANA,
             "WORDFREQ_JA": tmp_path / "large_ja.msgpack",
             "TATOEBA_JPN": tmp_path / "jpn_indices.csv", "JOYO_PDF": tmp_path / "joyo.pdf",
             "FREEDICT_ENG_SPA": _freedict(tmp_path / "eng-spa.tar.xz")}
    files["WORDFREQ_JA"].write_bytes(_msgpack([{"format": "cB"}]))
    files["TATOEBA_JPN"].write_bytes("1\t2\t毎日\n".encode())
    files["JOYO_PDF"].write_bytes(b"%PDF")
    for name, v in files.items():
        monkeypatch.setattr(paths, name, v)
    vocab = {n: _anki_deck(tmp_path / f"n{n}.anki",
                           [(t, r) for (t, r) in fixture.MINI_VOCAB] if n == 5 else [])
             for n in (5, 4, 3, 2, 1)}
    monkeypatch.setattr(paths, "JLPT_VOCAB", vocab)
    monkeypatch.setattr(joyo_table, "load_pdf", lambda p: {})
    table = tmp_path / "glosses.tsv"
    table.write_bytes(fixture.MINI_GLOSSES_ES.read_bytes())
    monkeypatch.setattr(paths, "EXAMPLE_GLOSSES_ES", table)
    seen = {}

    def fake(cards, *a, **kw):
        seen["cards"], seen["words"] = cards, kw.get("words")
        raise _Stop()

    monkeypatch.setattr(build, "build_pack", fake)
    return {"seen": seen, "table": table}


def test_cli_build_example_words(word_inputs, tmp_path, capsys):
    with pytest.raises(_Stop):
        cli.main(["build", "--jlpt", "5", "--lang", "es", "--common-readings", "--example-words",
                  "--es-glosses", str(word_inputs["table"]), "-o", str(tmp_path / "x.tgc")])
    seen = word_inputs["seen"]
    by = {c.char: ws for c, ws in zip(seen["cards"], seen["words"])}
    assert [(w.text, w.gloss) for w in by["火"]] == [("火山", "volcán"), ("火", "fuego")]
    assert [(w.text, w.gloss) for w in by["月"]] == [("今月", ""), ("月", "")]
    out = capsys.readouterr().out
    assert "example words: 6 for 3 cards" in out and "glosses for 4 of 6" in out


def test_cli_build_without_flag_has_no_words(word_inputs, tmp_path):
    with pytest.raises(_Stop):
        cli.main(["build", "--jlpt", "5", "--common-readings", "-o", str(tmp_path / "x.tgc")])
    assert word_inputs["seen"]["words"] is None


def test_cli_example_words_needs_fetch(word_inputs, monkeypatch, tmp_path, capsys):
    monkeypatch.setattr(paths, "JMDICT_FURIGANA", tmp_path / "missing.txt")
    rc = cli.main(["build", "--jlpt", "5", "--example-words", "-o", str(tmp_path / "x.tgc")])
    assert rc == 2 and "fetch --readings" in capsys.readouterr().err


def test_cli_example_words_not_for_wanikani(tmp_path, capsys):
    rc = cli.main(["build", "--wanikani-token", "x", "--example-words",
                   "-o", str(tmp_path / "x.tgc")])
    assert rc == 2 and "WaniKani" in capsys.readouterr().err


def test_cli_sync_glosses(word_inputs, capsys):
    table = word_inputs["table"]
    before = table.read_bytes()
    assert cli.main(["sync-glosses", "--jlpt", "5", "--es-glosses", str(table)]) == 0
    after = table.read_bytes()
    assert after.startswith(before)
    added = after[len(before):].decode().splitlines()
    # Only 月 つき was missing (今月 is a draft row, 火曜日 a skip row).
    assert added == ["N5\t月\t月\tつき\tmoon; month\tluna\tdraft\t"
                     "CHECK: FreeDict suggestion (sync-glosses), not reviewed"]
    out = capsys.readouterr().out
    assert "火 on カ: 火曜日 かようび is skipped, picked 火山 かざん" in out
    assert "appended 1 draft row(s)" in out
    assert "N5         3      6      6    4      2        0" in out
    # A second run appends nothing; --dry-run never writes.
    assert cli.main(["sync-glosses", "--jlpt", "5", "--es-glosses", str(table)]) == 0
    assert table.read_bytes() == after
    assert "appended 0 draft row(s)" in capsys.readouterr().out
    fresh = table.with_name("fresh.tsv")
    assert cli.main(["sync-glosses", "--jlpt", "5", "--es-glosses", str(fresh), "--dry-run"]) == 0
    assert not fresh.exists() and "would append 6 draft row(s)" in capsys.readouterr().out


def test_cli_sync_glosses_needs_a_selection(capsys):
    assert cli.main(["sync-glosses"]) == 2
    assert "--jlpt" in capsys.readouterr().err
