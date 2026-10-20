"""`build --meaning-order`: English meanings by the learner signal, Spanish ones
in the order of the English ones, and the overrides. The dictionary, JMdict
entries, vocabulary lists and kanji here are hand-written miniatures in the
sources' formats (FreeDict TEI, JMdict, Anki decks, KANJIDIC2) with invented
glosses, not copied from them."""

import io
import json
import sqlite3
import tarfile

import pytest

from tangocho_pack import build, cli, fetch, paths
from tangocho_pack.distractors import meaning_distractors
from tangocho_pack.kanjidic import Card, RawKanji, make_cards, parse_kanjidic
from tangocho_pack.meanings import (
    MIN_SCORE,
    EnglishOrder,
    MeaningOrder,
    Override,
    VocabWord,
    apply_override,
    check_overrides,
    cognate,
    en_heads,
    en_variants,
    es_heads,
    es_variants,
    gloss_match,
    gloss_tokens,
    load_freedict,
    load_learner_vocab,
    load_overrides,
    norm_en,
    norm_es,
    parse_jmdict_glosses,
    reorder_raw,
    same_stem,
)
from tangocho_pack.readings import load_jlpt_vocab

# --- a miniature FreeDict TEI --------------------------------------------------------

ENTRIES = {
    "hold": ["tener", "sostener", "agarrar"],
    "have": ["tener", "poseer"],
    "boundary": ["frontera", "límite"],
    "below": ["abajo", "bajo"],
    "under": ["debajo de", "bajo", "abajo"],
    "lend": ["prestar"],
    "go": ["ir"],
    "line": ["la línea", "fila"],
    "officer": ["oficial"],
    "bright": ["claro", "brillante"],
    "rise": ["levantarse"],
    "bird": ["pájaro", "ave"],
    "i": ["yo"],
    "private": ["privado"],
}


def tei(entries=ENTRIES) -> bytes:
    body = "".join(
        f'<entry><form><orth>{en}</orth></form><gramGrp><pos>v</pos></gramGrp>'
        f'<sense><cit type="trans" xml:lang="es">{"".join(f"<quote>{q}</quote>" for q in es)}'
        f'</cit><sense><def>…</def></sense></sense></entry>'
        for en, es in entries.items())
    return ('<?xml version="1.0" encoding="UTF-8"?>'
            '<TEI xmlns="http://www.tei-c.org/ns/1.0"><teiHeader/><text><body>'
            f'{body}</body></text></TEI>').encode()


def tar_xz(path, data: bytes):
    with tarfile.open(path, "w:xz") as t:
        info = tarfile.TarInfo("eng-spa/eng-spa.tei")
        info.size = len(data)
        t.addfile(info, io.BytesIO(data))
        info = tarfile.TarInfo("eng-spa/COPYING")
        info.size = 2
        t.addfile(info, io.BytesIO(b"cc"))
    return path


@pytest.fixture(scope="module")
def order():
    return MeaningOrder(load_freedict(tei()))


# --- normalisation ------------------------------------------------------------------

def test_normalisation():
    assert norm_en("To hold (in one's hand)") == "hold"
    assert norm_en("to be crowned with") == "crowned with"
    assert norm_en("Inst.") == "inst"
    assert norm_es("La línea (texto)") == "linea"
    assert norm_es("llevar (en coche") == "llevar"      # a gloss KANJIDIC2 split mid-parenthesis
    assert es_variants("levantarse") == ["levantarse", "levantar"]
    assert es_variants("debajo de") == ["debajo de", "debajo"]
    known = {"go", "place", "crown"}
    assert en_variants("going", known) == ["going", "go"]
    assert en_variants("places", known) == ["places", "place"]
    assert en_variants("go", known) == ["go"]
    assert en_heads("counter for days") == {"counter"}
    assert en_heads("military officer") == {"military", "officer"}
    assert es_heads("contador para casas") == {"contador"}
    assert es_heads("oficial militar") == {"oficial", "militar"}


def test_related_word_tests():
    assert same_stem("prestamo", "prestar") and same_stem("eterno", "eternidad")
    assert not same_stem("pesado", "peso") and not same_stem("casa", "casamiento")
    assert cognate("matrimonio", "matrimony") and cognate("institución", "institution")
    assert not cognate("mesa", "mesa ")  # too short
    assert not cognate("persona", "parade")


# --- the dictionary -----------------------------------------------------------------

def test_load_freedict_tei_and_archive(tmp_path):
    d = load_freedict(tei())
    assert d.get("hold") == {"tener", "sostener", "agarrar"}
    assert d.get("line") == {"linea", "fila"}                    # article and accent gone
    assert d.get("under") >= {"debajo de", "debajo"}             # with its variants
    assert d.get("rise") >= {"levantarse", "levantar"}
    assert d.rev["tener"] == {"hold", "have"}
    archived = load_freedict(tar_xz(tmp_path / "eng-spa.src.tar.xz", tei()))
    assert archived.trans == d.trans


def test_load_freedict_archive_without_tei(tmp_path):
    p = tmp_path / "x.tar.xz"
    with tarfile.open(p, "w:xz") as t:
        info = tarfile.TarInfo("README")
        info.size = 0
        t.addfile(info, io.BytesIO(b""))
    with pytest.raises(ValueError):
        load_freedict(p)


# --- ordering -----------------------------------------------------------------------

def test_translations_follow_the_english_order(order):
    # An invented kanji: "hold; have" in English, the Spanish list in another order.
    es = ["periodo", "posesión", "poseer", "sostener", "puesto"]
    assert order.order(es, ["hold", "have"]) == ["sostener", "poseer", "periodo", "posesión",
                                                 "puesto"]


def test_ties_and_unmatched_keep_kanjidic2_order(order):
    assert order.order(["borde", "agarrar", "tener", "ámbito"], ["hold"]) == [
        "agarrar", "tener", "borde", "ámbito"]
    assert order.order(["borde", "ámbito"], ["world"]) == ["borde", "ámbito"]


def test_same_word_accents_and_lemmas(order):
    assert order.match("Japón", ["day", "Japan"]) == (2, "cognate")   # not the same word
    assert order.match("hospital", ["hospital"]) == (0, "translation")
    assert order.match("la línea (texto)", ["row", "line"]) == (1, "translation")
    assert order.match("ir", ["going", "journey"]) == (0, "translation")    # going -> go


def test_related_forms_count_one_position_later(order):
    m = order.match("préstamo", ["lend"])
    assert m == (1, "stem")
    # A translation of the second English gloss ties with a related form of the
    # first: KANJIDIC2's order decides.
    assert order.order(["préstamo", "frontera"], ["lend", "boundary"]) == ["préstamo", "frontera"]
    assert order.order(["frontera", "préstamo"], ["lend", "boundary"]) == ["frontera", "préstamo"]
    # ... and a translation of the same English gloss comes first.
    assert order.order(["préstamo", "prestar"], ["lend"]) == ["prestar", "préstamo"]


def test_stems_and_cognates(order):
    assert order.match("matrimonio", ["matrimony"]) == (1, "cognate")
    # Even next to the dictionary's own translations of the English word.
    assert order.match("brillante", ["bright"]) == (0, "translation")
    assert order.match("brillo", ["bright"]) == (1, "stem")          # brillo ~ brillante
    assert order.match("claridad", ["bright"]) is None


def test_head_words_and_synonyms(order):
    assert order.match("pájaro grande", ["large bird"]) == (1, "head")
    assert order.match("oficial militar", ["military officer"]) == (1, "cognate")
    # debajo translates "under", which shares abajo and bajo with "below".
    assert order.match("debajo", ["below"]) == (1, "synonym")
    assert order.order(["superior", "debajo"], ["below", "superior"]) == ["superior", "debajo"]


# --- English: the learner signal -----------------------------------------------------------

JMDICT = """<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE JMdict [
<!ELEMENT JMdict (entry*)>
<!ENTITY n "noun (common) (futsuumeishi)">
<!ENTITY v5t "Godan verb with 'tsu' ending">
]>
<JMdict>
<entry><ent_seq>1</ent_seq><k_ele><keb>私</keb></k_ele>
<r_ele><reb>わたし</reb></r_ele><r_ele><reb>わたくし</reb></r_ele>
<sense><pos>&n;</pos><gloss>I</gloss><gloss>me</gloss></sense>
<sense><stagr>わたくし</stagr><gloss>privacy</gloss></sense></entry>
<entry><ent_seq>2</ent_seq><k_ele><keb>私立</keb></k_ele><r_ele><reb>しりつ</reb></r_ele>
<sense><gloss>private (establishment)</gloss></sense></entry>
<entry><ent_seq>3</ent_seq><k_ele><keb>持つ</keb></k_ele><r_ele><reb>もつ</reb></r_ele>
<sense><pos>&v5t;</pos><gloss>to hold (in one's hand)</gloss><gloss>to take</gloss></sense>
<sense><gloss>to have</gloss><gloss>to own</gloss></sense></entry>
<entry><ent_seq>4</ent_seq><k_ele><keb>機械</keb></k_ele><r_ele><reb>きかい</reb></r_ele>
<sense><gloss>machine</gloss></sense></entry>
</JMdict>"""


def anki_deck(path, rows):
    """A miniature Anki 1.x deck: the two tables load_jlpt_vocab reads."""
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


VOCAB = {5: [("私", "わたし"), ("持つ", "もつ")], 4: [], 3: [("私立", "しりつ")], 2: [],
         1: [("機械", "きかい")]}


def vocab_files(tmp_path):
    return {n: anki_deck(tmp_path / f"n{n}.anki", rows) for n, rows in VOCAB.items()}


def test_gloss_tokens_and_match():
    assert gloss_tokens("to come (spatially or temporally)") == gloss_tokens("come") == ("com",)
    assert gloss_tokens("going") == gloss_tokens("to go") == ("go",)
    assert gloss_tokens("in") == ("in",)                    # only small words: kept
    assert gloss_match(gloss_tokens("next"), gloss_tokens("next")) == 1.0
    assert gloss_match(gloss_tokens("next"), gloss_tokens("next week")) == 0.25
    assert gloss_match(gloss_tokens("next"), gloss_tokens("year after next")) == 0.125
    assert gloss_match(gloss_tokens("original"), gloss_tokens("origin")) == 0.5
    assert gloss_match(gloss_tokens("machine"), gloss_tokens("mechanism")) == 0.0


def test_parse_jmdict_glosses_restrictions():
    g = parse_jmdict_glosses(JMDICT.encode())
    assert g[("私", "わたし")] == [["I", "me"]]
    assert g[("私", "わたくし")] == [["I", "me"], ["privacy"]]   # stagr
    assert parse_jmdict_glosses(JMDICT.encode(), {"機械"}).keys() == {("機械", "きかい")}


def test_load_learner_vocab(tmp_path):
    vocab = load_learner_vocab(load_jlpt_vocab(vocab_files(tmp_path)), JMDICT.encode())
    by = {v.word: v for v in vocab}
    assert by["私"].level == 5 and by["私"].senses == (("I", "me"),)
    assert by["機械"].level == 1 and set(by) == {"私", "持つ", "私立", "機械"}


def test_english_order_scores():
    eo = EnglishOrder([VocabWord("私", "わたし", 5, (("I", "me"),)),
                       VocabWord("私立", "しりつ", 3, (("private (establishment)",),)),
                       VocabWord("機械", "きかい", 1, (("machine",),))])
    # N5 word of the kanji alone: 16; second gloss 16 / 1.2; N3 two-kanji word: 4 / 2.
    assert eo.scores("私", ["private", "I", "me"]) == pytest.approx([2.0, 16.0, 16 / 1.2])
    assert eo.order("私", ["private", "I", "me"]) == ["I", "me", "private"]
    # An N1 two-kanji word gives 1.5 / 2 < MIN_SCORE: no evidence, KANJIDIC2's order.
    assert 1.5 / 2 < MIN_SCORE
    assert eo.order("機", ["loom", "machine"]) == ["loom", "machine"]
    assert eo.order("持", ["hold", "have"]) == ["hold", "have"]       # no words: unchanged


# --- overrides ---------------------------------------------------------------------------

def test_load_and_apply_overrides(tmp_path):
    f = tmp_path / "o.tsv"
    f.write_text("# comment\n持\tes\tposesión\tsostener; periodo\tbecause\n"
                 "私\ten\tI\t\tthe N5 word\n", encoding="utf-8")
    ov = load_overrides(f)
    assert ov[("持", "es")].order == ("posesión", "sostener", "periodo")
    assert apply_override(["sostener", "periodo", "posesión", "x"], ov[("持", "es")]) == [
        "posesión", "sostener", "periodo", "x"]
    assert apply_override(["me", "I"], ov[("私", "en")]) == ["I", "me"]
    assert apply_override(["a"], None) == ["a"]


@pytest.mark.parametrize("line", ["持\tes\tposesión\treason",            # 4 columns
                                  "持\tes\t\t\treason",                   # no meaning
                                  "持\txx\tposesión\t\treason",           # language
                                  "持つ\tes\tposesión\t\treason",         # not one kanji
                                  "持\tes\tposesión\tposesión\treason",   # listed twice
                                  "持\tes\tposesión\t\t"])                # no reason
def test_overrides_reject_malformed_lines(tmp_path, line):
    f = tmp_path / "o.tsv"
    f.write_text(line + "\n", encoding="utf-8")
    with pytest.raises(ValueError):
        load_overrides(f)


def test_overrides_reject_a_second_line_for_a_kanji(tmp_path):
    f = tmp_path / "o.tsv"
    f.write_text("持\tes\ta\t\tr\n持\tes\tb\t\tr\n", encoding="utf-8")
    with pytest.raises(ValueError):
        load_overrides(f)


def test_check_overrides_only_kanjidic2_meanings():
    raw = [RawKanji(0x6301, {"en": ["hold"], "es": ["sostener", "periodo"]}, [], [], 0, 3, 9, 0)]
    ok = {("持", "es"): Override("持", "es", ("periodo",), "r", 1)}
    assert check_overrides(ok, raw) == []
    bad = {("持", "es"): Override("持", "es", ("tener",), "r", 1),
           ("持", "fr"): Override("持", "fr", ("tenir",), "r", 2),
           ("私", "en"): Override("私", "en", ("I",), "r", 3)}
    errors = check_overrides(bad, raw)
    assert len(errors) == 3 and "'tener'" in errors[0]


def test_committed_overrides_parse():
    ov = load_overrides(paths.MEANING_OVERRIDES)
    assert 0 < len(ov) <= 40
    assert {lang for _, lang in ov} <= {"en", "es"}


@pytest.mark.skipif(not paths.KANJIDIC_GZ.exists(), reason="run `tangocho-pack fetch`")
def test_committed_overrides_use_kanjidic2_meanings():
    assert check_overrides(load_overrides(paths.MEANING_OVERRIDES),
                           parse_kanjidic(paths.KANJIDIC_GZ)) == []


# --- all together ------------------------------------------------------------------------

def test_reorder_raw_english_then_spanish_then_overrides(order):
    eo = EnglishOrder([VocabWord("私", "わたし", 5, (("I", "me"),))])
    shi = RawKanji(0x79C1, {"en": ["private", "I", "me"], "es": ["privado", "yo"],
                            "fr": ["privé", "je"]}, [], [], 0, 6, 7, 0)
    ji = RawKanji(0x6301, {"en": ["hold", "have"], "es": ["periodo", "sostener", "poseer"]},
                  [], [], 0, 3, 9, 0)
    no_es = RawKanji(0x4E00, {"en": ["one"]}, [], [], 0, 1, 1, 0)
    out, st = reorder_raw([shi, ji, no_es], "es", eo, order)
    assert out[0].meanings["en"] == ["I", "me", "private"]
    assert out[0].meanings["es"] == ["yo", "privado"]               # by the new English order
    assert out[0].meanings["fr"] == ["privé", "je"]
    assert out[1].meanings["es"] == ["sostener", "poseer", "periodo"]
    assert shi.meanings["en"] == ["private", "I", "me"]              # the input is not changed
    assert out[2] is no_es
    assert (st.kanji, st.first_changed, st.glosses, st.matched) == (2, 2, 5, 4)
    ov = {("持", "es"): Override("持", "es", ("periodo",), "r", 1),
          ("私", "en"): Override("私", "en", ("me",), "r", 2)}
    out, st = reorder_raw([shi, ji], "es", eo, order, ov)
    assert out[1].meanings["es"] == ["periodo", "sostener", "poseer"] and st.overridden == 1
    assert out[0].meanings["en"] == ["me", "I", "private"]
    out, st = reorder_raw([shi, ji], "en", eo, None, ov)
    assert out[0].meanings["en"] == ["me", "I", "private"]
    assert out[0].meanings["es"] == ["privado", "yo"]
    assert (st.kanji, st.first_changed, st.overridden) == (2, 1, 1)


# --- cards and distractors ------------------------------------------------------------

def test_cap_applies_after_reordering_and_valid_meanings(order):
    k = RawKanji(0x6301, {"en": ["hold", "have"], "es": ["periodo", "posesión", "sostener"]},
                 ["ジ"], [], 0, 3, 9, 0)
    plain = make_cards([k], "es")[0]
    assert plain.meanings == ["periodo", "posesión"] and plain.valid_meanings == []
    out, _ = reorder_raw([k], "es", None, order)
    card = make_cards(out, "es", keep_all_meanings=True)[0]
    assert card.meanings == ["sostener", "periodo"]
    assert card.valid_meanings == ["sostener", "periodo", "posesión"]


def test_meaning_distractors_avoid_unstored_meanings():
    def cards(valid):
        return [Card(1, ["sostener", "periodo"], [], [], jlpt=5, strokes=9,
                     valid_meanings=["sostener", "periodo", "posesión"] if valid else []),
                Card(2, ["posesión"], [], [], jlpt=5, strokes=9),   # shows an unstored meaning of 1
                Card(3, ["mesa"], [], [], jlpt=5, strokes=9,
                     valid_meanings=["mesa", "sostener"] if valid else []),  # 1's first is 3's
                Card(4, ["silla"], [], [], jlpt=5, strokes=9)]
    visual = [(1.0, 0.0)] * 4
    assert sorted(meaning_distractors(cards(False), visual)[0]) == [1, 2, 3]
    with_valid = meaning_distractors(cards(True), visual)
    assert with_valid[0] == [3]
    assert 0 not in with_valid[1] and 0 not in with_valid[2]


# --- the CLI --------------------------------------------------------------------------

class _Stop(Exception):
    pass


KD2 = """<?xml version="1.0" encoding="UTF-8"?><kanjidic2>
<character><literal>持</literal><codepoint><cp_value cp_type="ucs">6301</cp_value></codepoint>
<misc><grade>3</grade><stroke_count>9</stroke_count></misc>
<reading_meaning><rmgroup><reading r_type="ja_on">ジ</reading>
<meaning>hold</meaning><meaning>have</meaning>
<meaning m_lang="es">periodo</meaning><meaning m_lang="es">posesión</meaning>
<meaning m_lang="es">sostener</meaning>
<meaning m_lang="fr">porter</meaning><meaning m_lang="fr">tenir</meaning>
</rmgroup></reading_meaning></character>
<character><literal>私</literal><codepoint><cp_value cp_type="ucs">79c1</cp_value></codepoint>
<misc><grade>6</grade><stroke_count>7</stroke_count></misc>
<reading_meaning><rmgroup><reading r_type="ja_on">シ</reading>
<meaning>private</meaning><meaning>I</meaning><meaning>me</meaning>
<meaning m_lang="es">privado</meaning><meaning m_lang="es">yo</meaning>
<meaning m_lang="fr">privé</meaning><meaning m_lang="fr">je</meaning>
</rmgroup></reading_meaning></character>
</kanjidic2>"""


@pytest.fixture
def fake_inputs(monkeypatch, tmp_path):
    kd = tmp_path / "kanjidic2.xml"
    kd.write_text(KD2, encoding="utf-8")
    levels = tmp_path / "kanji.json"
    levels.write_text(json.dumps({"持": {"jlpt_new": 4}, "私": {"jlpt_new": 4}}), encoding="utf-8")
    jm = tmp_path / "JMdict_e"
    jm.write_text(JMDICT, encoding="utf-8")
    ov = tmp_path / "overrides.tsv"
    ov.write_text("# none\n", encoding="utf-8")
    monkeypatch.setattr(paths, "KANJIDIC_GZ", kd)
    monkeypatch.setattr(paths, "KANJI_JSON", levels)
    monkeypatch.setattr(paths, "JMDICT_GZ", jm)
    monkeypatch.setattr(paths, "JLPT_VOCAB", vocab_files(tmp_path))
    monkeypatch.setattr(paths, "MEANING_OVERRIDES", ov)
    monkeypatch.setattr(paths, "FREEDICT_ENG_SPA", tar_xz(tmp_path / "eng-spa.src.tar.xz", tei()))
    seen = {}

    def fake(cards, *a, **kw):
        seen["cards"] = cards
        raise _Stop()

    monkeypatch.setattr(build, "build_pack", fake)
    return seen


def build_cards(seen, tmp_path, *args):
    with pytest.raises(_Stop):
        cli.main(["build", "--jlpt", "4", *args, "-o", str(tmp_path / "x.tgc")])
    return {c.char: c for c in seen["cards"]}


def test_cli_meaning_order(fake_inputs, tmp_path, capsys):
    cards = build_cards(fake_inputs, tmp_path, "--lang", "es", "--meaning-order")
    assert cards["持"].meanings == ["sostener", "periodo"]
    assert cards["持"].valid_meanings == ["sostener", "periodo", "posesión"]
    assert cards["私"].meanings == ["yo", "privado"]
    assert "first meaning changed for 2 of 2 cards; the pack keeps other meanings for 1" in \
        capsys.readouterr().out


def test_cli_meaning_order_english(fake_inputs, tmp_path, capsys):
    cards = build_cards(fake_inputs, tmp_path, "--lang", "en", "--meaning-order")
    assert cards["私"].meanings == ["I", "me", "private"]
    assert cards["持"].meanings == ["hold", "have"]
    assert "meaning order (en): first meaning changed for 1 of 2 cards" in capsys.readouterr().out


def test_cli_meaning_order_overrides(fake_inputs, tmp_path):
    paths.MEANING_OVERRIDES.write_text("持\tes\tposesión\t\tinvented\n", encoding="utf-8")
    cards = build_cards(fake_inputs, tmp_path, "--lang", "es", "--meaning-order")
    assert cards["持"].meanings == ["posesión", "sostener"]


def test_cli_meaning_order_rejects_bad_overrides(fake_inputs, tmp_path, capsys):
    paths.MEANING_OVERRIDES.write_text("持\tes\ttener\t\tnot a KANJIDIC2 meaning\n",
                                       encoding="utf-8")
    rc = cli.main(["build", "--jlpt", "4", "--lang", "es", "--meaning-order",
                   "-o", str(tmp_path / "x.tgc")])
    assert rc == 2 and "'tener'" in capsys.readouterr().err
    assert "cards" not in fake_inputs


def test_cli_default_keeps_kanjidic2_order(fake_inputs, tmp_path):
    cards = build_cards(fake_inputs, tmp_path, "--lang", "es")
    assert cards["持"].meanings == ["periodo", "posesión"] and cards["持"].valid_meanings == []
    cards = build_cards(fake_inputs, tmp_path, "--lang", "en")
    assert cards["私"].meanings == ["private", "I", "me"]


def test_cli_meaning_order_french_keeps_its_order(fake_inputs, tmp_path, monkeypatch, capsys):
    monkeypatch.setattr(paths, "FREEDICT_ENG_SPA", tmp_path / "missing.tar.xz")  # not read
    cards = build_cards(fake_inputs, tmp_path, "--lang", "fr", "--meaning-order")
    # Not reordered, but every meaning still keeps meaning distractors away.
    assert cards["持"].meanings == ["porter", "tenir"]
    assert cards["持"].valid_meanings == ["porter", "tenir"]
    assert "no dictionary" in capsys.readouterr().err


@pytest.mark.parametrize("missing", ["FREEDICT_ENG_SPA", "JMDICT_GZ"])
def test_cli_meaning_order_needs_fetch(fake_inputs, monkeypatch, tmp_path, capsys, missing):
    monkeypatch.setattr(paths, missing, tmp_path / "missing")
    rc = cli.main(["build", "--jlpt", "4", "--lang", "es", "--meaning-order",
                   "-o", str(tmp_path / "x.tgc")])
    assert rc == 2
    assert "fetch --readings --meanings" in capsys.readouterr().err
    assert "cards" not in fake_inputs


def test_cli_meaning_order_not_for_wanikani(tmp_path, capsys):
    rc = cli.main(["build", "--wanikani-token", "x", "--meaning-order",
                   "-o", str(tmp_path / "x.tgc")])
    assert rc == 2
    assert "WaniKani" in capsys.readouterr().err


def test_fetch_pins_the_dictionary():
    (url, dest), = fetch.MEANINGS_SOURCES
    assert dest == paths.FREEDICT_ENG_SPA
    assert fetch.FREEDICT_ENG_SPA_RELEASE in url and url.endswith(dest.name)
    assert len(fetch.SHA256[dest]) == 64


# --- the real data (skipped without `fetch --readings --meanings`) ------------------------

REAL = [paths.KANJIDIC_GZ, paths.FREEDICT_ENG_SPA, paths.JMDICT_GZ, *paths.JLPT_VOCAB.values()]


@pytest.mark.skipif(not all(p.exists() for p in REAL),
                    reason="run `tangocho-pack fetch --readings --meanings`")
def test_real_data_sanity():
    raw = parse_kanjidic(paths.KANJIDIC_GZ)
    eo = EnglishOrder(load_learner_vocab(load_jlpt_vocab(paths.JLPT_VOCAB), paths.JMDICT_GZ))
    mo = MeaningOrder(load_freedict(paths.FREEDICT_ENG_SPA))
    ov = load_overrides(paths.MEANING_OVERRIDES)
    some = [k for k in raw if chr(k.codepoint) in "持界題食書下人私明原機申中"]
    out, _ = reorder_raw(some, "es", eo, mo, ov)
    first = {chr(k.codepoint): (k.meanings["en"][0], k.meanings["es"][0]) for k in out}
    assert first == {"持": ("hold", "sostener"), "界": ("world", "frontera"),
                     "題": ("subject", "tema"), "食": ("eat", "comer"), "書": ("write", "escribir"),
                     "下": ("down", "debajo"), "人": ("person", "persona"), "私": ("I", "yo"),
                     "明": ("light", "luz"), "原": ("original", "origen"),
                     "機": ("machine", "máquina"), "申": ("have the honor to", "decir"),
                     "中": ("middle", "dentro")}
    for k, o in zip(some, out):
        for lang in ("en", "es"):
            assert sorted(k.meanings[lang]) == sorted(o.meanings[lang])
