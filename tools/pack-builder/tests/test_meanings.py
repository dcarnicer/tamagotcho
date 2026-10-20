"""`build --meaning-order`: Spanish meanings in the order of the English ones.
The dictionary and kanji here are hand-written miniatures in the sources'
formats (FreeDict TEI, KANJIDIC2) with invented glosses, not copied from them."""

import io
import json
import tarfile

import pytest

from tangocho_pack import build, cli, fetch, paths
from tangocho_pack.distractors import meaning_distractors
from tangocho_pack.kanjidic import Card, RawKanji, make_cards, parse_kanjidic
from tangocho_pack.meanings import (
    MeaningOrder,
    cognate,
    en_heads,
    en_variants,
    es_heads,
    es_variants,
    load_freedict,
    norm_en,
    norm_es,
    reorder_raw,
    same_stem,
)

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


def test_reorder_raw_only_reorders(order):
    k = RawKanji(0x6301, {"en": ["hold", "have"], "es": ["periodo", "sostener", "poseer"],
                          "fr": ["porter", "tenir"]}, [], [], 0, 3, 9, 0)
    no_es = RawKanji(0x4E00, {"en": ["one"]}, [], [], 0, 1, 1, 0)
    out, st = reorder_raw([k, no_es], order, "es")
    assert out[0].meanings["es"] == ["sostener", "poseer", "periodo"]
    assert out[0].meanings["en"] == ["hold", "have"] and out[0].meanings["fr"] == ["porter", "tenir"]
    assert k.meanings["es"] == ["periodo", "sostener", "poseer"]     # the input is not changed
    assert out[1] is no_es
    assert (st.kanji, st.first_changed, st.glosses, st.matched) == (1, 1, 3, 2)


# --- cards and distractors ------------------------------------------------------------

def test_cap_applies_after_reordering_and_valid_meanings(order):
    k = RawKanji(0x6301, {"en": ["hold", "have"], "es": ["periodo", "posesión", "sostener"]},
                 ["ジ"], [], 0, 3, 9, 0)
    plain = make_cards([k], "es")[0]
    assert plain.meanings == ["periodo", "posesión"] and plain.valid_meanings == []
    out, _ = reorder_raw([k], order, "es")
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
</kanjidic2>"""


@pytest.fixture
def fake_inputs(monkeypatch, tmp_path):
    kd = tmp_path / "kanjidic2.xml"
    kd.write_text(KD2, encoding="utf-8")
    levels = tmp_path / "kanji.json"
    levels.write_text(json.dumps({"持": {"jlpt_new": 4}}), encoding="utf-8")
    monkeypatch.setattr(paths, "KANJIDIC_GZ", kd)
    monkeypatch.setattr(paths, "KANJI_JSON", levels)
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
    return seen["cards"]


def test_cli_meaning_order(fake_inputs, tmp_path, capsys):
    (card,) = build_cards(fake_inputs, tmp_path, "--lang", "es", "--meaning-order")
    assert card.meanings == ["sostener", "periodo"]
    assert card.valid_meanings == ["sostener", "periodo", "posesión"]
    assert "first meaning changed for 1 of 1 cards; the pack keeps other meanings for 1" in \
        capsys.readouterr().out


def test_cli_default_keeps_kanjidic2_order(fake_inputs, tmp_path):
    (card,) = build_cards(fake_inputs, tmp_path, "--lang", "es")
    assert card.meanings == ["periodo", "posesión"] and card.valid_meanings == []


@pytest.mark.parametrize("lang, meanings", [("en", ["hold", "have"]), ("fr", ["porter", "tenir"])])
def test_cli_meaning_order_other_languages_keep_their_order(fake_inputs, tmp_path, monkeypatch,
                                                            capsys, lang, meanings):
    monkeypatch.setattr(paths, "FREEDICT_ENG_SPA", tmp_path / "missing.tar.xz")  # not read
    (card,) = build_cards(fake_inputs, tmp_path, "--lang", lang, "--meaning-order")
    # Not reordered, but every meaning still keeps meaning distractors away.
    assert card.meanings == meanings and card.valid_meanings == meanings
    out = capsys.readouterr()
    assert ("nothing to reorder" in out.out) if lang == "en" else ("no dictionary" in out.err)


def test_cli_meaning_order_needs_fetch(fake_inputs, monkeypatch, tmp_path, capsys):
    monkeypatch.setattr(paths, "FREEDICT_ENG_SPA", tmp_path / "missing.tar.xz")
    rc = cli.main(["build", "--jlpt", "4", "--lang", "es", "--meaning-order",
                   "-o", str(tmp_path / "x.tgc")])
    assert rc == 2
    assert "fetch --meanings" in capsys.readouterr().err
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


# --- the real data (skipped without `fetch --meanings`) ----------------------------------

@pytest.mark.skipif(not (paths.KANJIDIC_GZ.exists() and paths.FREEDICT_ENG_SPA.exists()),
                    reason="run `tangocho-pack fetch --meanings`")
def test_real_data_sanity():
    raw = [k for k in parse_kanjidic(paths.KANJIDIC_GZ) if chr(k.codepoint) in "持界題食書下人"]
    out, _ = reorder_raw(raw, MeaningOrder(load_freedict(paths.FREEDICT_ENG_SPA)), "es")
    first = {chr(k.codepoint): k.meanings["es"][0] for k in out}
    assert first == {"持": "sostener", "界": "frontera", "題": "tema", "食": "comer",
                     "書": "escribir", "下": "debajo", "人": "persona"}
    for k, o in zip(raw, out):
        assert sorted(k.meanings["es"]) == sorted(o.meanings["es"])
