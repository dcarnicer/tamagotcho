"""`build --common-readings`: reading scores from JMdict + JmdictFurigana +
wordfreq + Tatoeba, the Jōyō table parse, and the reordering. All inputs here
are hand-written miniatures in the sources' formats (no third-party data)."""

import json
import math

import pytest

from tangocho_pack import build, cli, joyo_table, paths
from tangocho_pack.joyo_table import JoyoReading, classify, parse_pages
from tangocho_pack.kanjidic import RawKanji, parse_kanjidic
from tangocho_pack.readings import (
    RANK_COMMON,
    RANK_NONE,
    UK_FACTOR,
    KanjiReadings,
    WordPair,
    bound_forms,
    corpus_weights,
    load_tatoeba_counts,
    load_wordfreq,
    match_level,
    order_by_score,
    pair_weight,
    parse_furigana,
    parse_jmdict,
    priority_rank,
    reorder_raw,
    score_readings,
    sound_variants,
    surface_forms,
)

# --- miniature inputs -----------------------------------------------------------

JMDICT = """<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE JMdict [
<!ELEMENT JMdict (entry*)>
<!ENTITY uk "word usually written using kana alone">
<!ENTITY rK "rarely used kanji form">
<!ENTITY v1 "Ichidan verb">
<!ENTITY v5k-s "Godan verb - Iku/Yuku special class">
<!ENTITY n "noun (common) (futsuumeishi)">
]>
<JMdict>
<entry><ent_seq>1</ent_seq>
<k_ele><keb>九つ</keb><ke_pri>ichi1</ke_pri></k_ele>
<r_ele><reb>ここのつ</reb><re_pri>ichi1</re_pri></r_ele>
<sense><pos>&n;</pos><gloss>nine</gloss></sense></entry>
<entry><ent_seq>2</ent_seq>
<k_ele><keb>九日</keb></k_ele>
<r_ele><reb>ここのか</reb></r_ele>
<sense><pos>&n;</pos><gloss>ninth day</gloss></sense></entry>
<entry><ent_seq>3</ent_seq>
<k_ele><keb>生きる</keb><ke_pri>ichi1</ke_pri><ke_pri>nf04</ke_pri></k_ele>
<k_ele><keb>活きる</keb><ke_inf>&rK;</ke_inf></k_ele>
<r_ele><reb>いきる</reb><re_pri>ichi1</re_pri><re_pri>nf04</re_pri></r_ele>
<sense><pos>&v1;</pos><gloss>to live</gloss></sense></entry>
<entry><ent_seq>4</ent_seq>
<k_ele><keb>行く</keb><ke_pri>ichi1</ke_pri></k_ele>
<r_ele><reb>いく</reb><re_pri>ichi1</re_pri></r_ele>
<r_ele><reb>ゆく</reb><re_pri>ichi1</re_pri></r_ele>
<sense><pos>&v5k-s;</pos><gloss>to go</gloss></sense></entry>
<entry><ent_seq>5</ent_seq>
<k_ele><keb>為る</keb></k_ele>
<r_ele><reb>する</reb></r_ele>
<sense><pos>&v1;</pos><misc>&uk;</misc><gloss>to do</gloss></sense></entry>
<entry><ent_seq>6</ent_seq>
<k_ele><keb>今日</keb><ke_pri>nf02</ke_pri></k_ele>
<k_ele><keb>今日わ</keb></k_ele>
<r_ele><reb>きょう</reb><re_pri>nf02</re_pri></r_ele>
<r_ele><reb>こんにち</reb><re_restr>今日</re_restr></r_ele>
<r_ele><reb>キョー</reb><re_nokanji/></r_ele>
<sense><pos>&n;</pos><gloss>today</gloss></sense></entry>
</JMdict>
"""

FURIGANA = "﻿九つ|ここのつ|0:ここの\n九日|ここのか|0:ここの;1:か\n生きる|いきる|0:い\n" \
           "今日|きょう|0-1:きょう\n今日|こんにち|0:こん;1:にち\n行く|いく|0:い\n行く|ゆく|0:ゆ\n"


def msgpack(v) -> bytes:
    """A tiny MessagePack encoder for the test lists (fixarray/fixmap/str8)."""
    if isinstance(v, list):
        assert len(v) < 16
        return bytes([0x90 | len(v)]) + b"".join(msgpack(x) for x in v)
    if isinstance(v, dict):
        return bytes([0x80 | len(v)]) + b"".join(msgpack(k) + msgpack(x) for k, x in v.items())
    b = v.encode("utf-8")
    return bytes([0xD9, len(b)]) + b


# --- JMdict, JmdictFurigana, wordfreq, Tatoeba ---------------------------------

def test_priority_rank_and_pair_weight():
    assert priority_rank(["nf03", "news1"]) == 1250
    assert priority_rank(["ichi1"]) == RANK_COMMON
    assert priority_rank([]) is None
    # A pair is as rare as its rarer half: a common spelling's untagged reading.
    assert pair_weight(["nf02"], []) == 1 / RANK_NONE
    assert pair_weight(["nf02"], ["nf02"]) == 1 / 750
    assert pair_weight(["ichi1"], ["ichi1"], usually_kana=True) == UK_FACTOR / RANK_COMMON


def test_parse_jmdict_pairs_and_skips():
    pairs = {(p.text, p.reading): p for p in parse_jmdict(JMDICT.encode())}
    assert ("活きる", "いきる") not in pairs              # rK kanji form
    assert ("今日わ", "こんにち") not in pairs            # re_restr to 今日
    assert ("今日", "キョー") not in pairs                # re_nokanji
    assert ("今日わ", "きょう") in pairs
    assert pairs[("生きる", "いきる")].pos == ("v1",)
    assert pairs[("行く", "ゆく")].pos == ("v5k-s",)
    assert pairs[("為る", "する")].weight == pytest.approx(UK_FACTOR / RANK_NONE)
    assert pairs[("今日", "こんにち")].weight == 1 / RANK_NONE


def test_parse_furigana_bom_and_spans():
    f = parse_furigana(FURIGANA.encode())
    assert f[("九つ", "ここのつ")] == [(0, 0, "ここの")]
    assert f[("今日", "きょう")] == [(0, 1, "きょう")]
    assert f[("今日", "こんにち")] == [(0, 0, "こん"), (1, 1, "にち")]


def test_load_wordfreq_cbpack():
    data = msgpack([{"format": "cB", "version": "1"}, ["の"], [], ["日本", "九つ"]])
    freq = load_wordfreq(data)
    assert freq["の"] == 1.0
    assert freq["日本"] == pytest.approx(10 ** -0.02)
    assert "x" not in freq
    with pytest.raises(ValueError):
        load_wordfreq(msgpack([["no header"]]))


def test_load_tatoeba_counts():
    text = ("1\t2\t上(うえ)[01]{上に} 上(うえ) 上(じょう) 何(#1234) 行く{行った}~\n"
            "3\t4\t上(うえ)|1 下\n")
    c = load_tatoeba_counts(text.encode())
    assert c[("上", "うえ")] == 3 and c[("上", "じょう")] == 1
    assert c[("何", None)] == 1 and c[("行く", None)] == 1 and c[("下", None)] == 1


def test_surface_forms_inflection_stems():
    assert surface_forms("生まれる", ["v1"]) >= {"生まれる", "生まれ"}
    assert surface_forms("行く", ["v5k-s"]) >= {"行か", "行き", "行く", "行け", "行こ", "行っ"}
    assert surface_forms("言う", ["v5u"]) >= {"言わ", "言い", "言え", "言っ"}
    assert surface_forms("高い", ["adj-i"]) >= {"高く", "高かっ", "高けれ"}
    assert surface_forms("九つ", ["n"]) == {"九つ"}


def test_corpus_weights_share_tokens_and_hold_back_lone_kanji():
    pairs = [WordPair("生まれる", "うまれる", 1 / RANK_COMMON, ("v1",)),
             WordPair("生まれ", "うまれ", 1 / RANK_COMMON, ("n",)),
             WordPair("上", "うえ", 1 / RANK_COMMON, ("n",)),
             WordPair("上", "じょう", 1 / RANK_COMMON, ("n",))]
    freq = {"生まれる": 1e-5, "生まれ": 4e-5, "上": 1e-3}
    w = {(p.text, p.reading): p.weight for p in corpus_weights(pairs, freq)}
    # 生まれ is shared by both words; 生まれる also has its own form.
    assert w[("生まれる", "うまれる")] == pytest.approx(1e-5 + 2e-5, rel=0.01)
    assert w[("生まれ", "うまれ")] == pytest.approx(2e-5, rel=0.01)
    # Without Tatoeba evidence a one-character form gets no corpus share.
    assert w[("上", "うえ")] < 1e-6
    # With it, 上's count is split by the Tatoeba readings (smoothed).
    counts = {("上", "うえ"): 18, ("上", "じょう"): 2}
    w = {(p.text, p.reading): p.weight for p in corpus_weights(pairs, freq, counts)}
    assert w[("上", "うえ")] == pytest.approx(1e-3 * (18 + 1) / 22, rel=0.01)
    assert w[("上", "じょう")] == pytest.approx(1e-3 * (2 + 1) / 22, rel=0.01)


# --- matching ---------------------------------------------------------------------

def test_sound_variants():
    assert sound_variants("ひと") == {"びと", "ぴと"}
    assert "がっ" in sound_variants("がく") and "かっ" in sound_variants("かく")
    assert sound_variants("ほん") >= {"ぼん", "ぽん"}


@pytest.mark.parametrize("rt,following,readings,best", [
    ("い", "きる", ["い.きる", "い.かす", "い.ける"], "い.きる"),     # 生きる
    ("い", "き物", ["い.きる", "い.かす", "い.ける"], "い.きる"),     # 生き物
    ("い", "け花", ["い.きる", "い.かす", "い.ける"], "い.ける"),     # 生け花
    ("いけ", "花", ["い.きる", "い.ける", "なま"], "い.ける"),         # 生花 (okurigana inside)
    ("ここの", "つ", ["ここの", "ここの.つ"], "ここの.つ"),           # 九つ
    ("ここの", "日", ["ここの", "ここの.つ"], "ここの"),              # 九日
    ("がっ", "校", ["ガク"], "ガク"),                                # 学校
    ("びと", "", ["ジン", "ひと"], "ひと"),                          # 人々
    ("ぽん", "", ["ホン", "もと"], "ホン"),                          # 一本
])
def test_match_level_picks_the_reading(rt, following, readings, best):
    levels = {r: match_level(rt, following, r) for r in readings}
    got = min((lv, i, r) for i, (r, lv) in enumerate(levels.items()) if lv is not None)[2]
    assert got == best


def test_match_level_rejects_other_readings():
    assert match_level("なま", "", "セイ") is None
    assert match_level("か", "", "ここの") is None


def test_score_readings_end_to_end():
    kanji = {ord("九"): KanjiReadings(["キュウ", "ク"], ["ここの", "ここの.つ"]),
             ord("今"): KanjiReadings(["コン", "キン"], ["いま"]),
             ord("日"): KanjiReadings(["ニチ", "ジツ"], ["ひ", "か"])}
    pairs = list(parse_jmdict(JMDICT.encode()))
    res = score_readings(kanji, pairs, parse_furigana(FURIGANA.encode()))
    nine = res.score[ord("九")]
    assert nine["ここの.つ"] == pytest.approx(1 / RANK_COMMON)   # 九つ
    assert nine["ここの"] == pytest.approx(1 / RANK_NONE)         # 九日
    assert res.of(ord("日"), "か") > 0 and res.of(ord("日"), "ひ") == 0
    # 今日 きょう is jukujikun (no per-kanji reading); こんにち is per kanji.
    assert res.of(ord("今"), "コン") == pytest.approx(1 / RANK_NONE)
    assert res.matched > 0


# --- ordering -----------------------------------------------------------------------

def test_bound_forms():
    assert bound_forms(["ひ", "び", "か"], {"び", "か"}) == {"び", "か"}
    assert bound_forms(["ここの", "ここの.つ"]) == {"ここの"}       # bare stem
    assert bound_forms(["おも.い", "おも", "え"]) == {"おも"}
    assert bound_forms(["から", "がら"]) == {"がら"}                 # rendaku form
    assert bound_forms(["した", "しも", "もと"]) == set()


def test_order_by_score_bound_tier_and_ties():
    sc = {"ここの": 0.6, "ここの.つ": 0.4}
    assert order_by_score(["ここの", "ここの.つ"], sc) == ["ここの", "ここの.つ"]
    assert order_by_score(["ここの", "ここの.つ"], sc, {"ここの"}) == ["ここの.つ", "ここの"]
    # Unscored readings keep their KANJIDIC2 order, after the scored ones.
    assert order_by_score(["a", "b", "c", "d"], {"c": 1.0}) == ["c", "a", "b", "d"]
    # Tier 0 (approved) before tier 1 whatever the score.
    assert order_by_score(["ダイ", "テイ"], {"ダイ": 9.0, "テイ": 1.0},
                          tier={"ダイ": 1, "テイ": 0}) == ["テイ", "ダイ"]


def test_reorder_raw_keeps_every_reading():
    from tangocho_pack.readings import ReadingScores

    raw = [RawKanji(ord("九"), {"en": ["nine"]}, ["キュウ", "ク"], ["ここの", "ここの.つ"],
                    4, 1, 2, 55),
           RawKanji(ord("弟"), {"en": ["brother"]}, ["テイ", "ダイ", "デ"], ["おとうと"],
                    4, 2, 7, 100)]
    scores = ReadingScores(score={ord("九"): {"ここの": 0.6, "ここの.つ": 0.4},
                                  ord("弟"): {"ダイ": 5.0, "デ": 2.0, "テイ": 1.0}})
    joyo = {"弟": [JoyoReading("テイ", False), JoyoReading("ダイ", True),
                   JoyoReading("デ", True), JoyoReading("おとうと", False)]}
    out, st = reorder_raw(raw, scores, joyo)
    assert out[0].kunyomi == ["ここの.つ", "ここの"]
    assert out[1].onyomi == ["テイ", "ダイ", "デ"]       # special readings after approved
    assert (st.first_kun_changed, st.first_on_changed) == (1, 0)
    for a, b in zip(raw, out):
        assert sorted(a.onyomi) == sorted(b.onyomi) and sorted(a.kunyomi) == sorted(b.kunyomi)
    # Without the table, frequency alone would ask ダイ.
    assert reorder_raw(raw, scores)[0][1].onyomi[0] == "ダイ"


def test_parse_kanjidic_records_bound_kun():
    xml = """<?xml version="1.0" encoding="UTF-8"?><kanjidic2>
<character><literal>日</literal><codepoint><cp_value cp_type="ucs">65e5</cp_value></codepoint>
<misc><grade>1</grade><stroke_count>4</stroke_count></misc><reading_meaning><rmgroup>
<reading r_type="ja_kun">ひ</reading><reading r_type="ja_kun">-び</reading>
<reading r_type="ja_kun">-か</reading><meaning>day</meaning></rmgroup></reading_meaning></character>
<character><literal>生</literal><codepoint><cp_value cp_type="ucs">751f</cp_value></codepoint>
<misc><grade>1</grade><stroke_count>5</stroke_count></misc><reading_meaning><rmgroup>
<reading r_type="ja_kun">なま</reading><reading r_type="ja_kun">なま-</reading>
<meaning>life</meaning></rmgroup></reading_meaning></character></kanjidic2>"""
    raw = {k.codepoint: k for k in parse_kanjidic(xml.encode())}
    assert raw[ord("日")].kunyomi == ["ひ", "び", "か"]
    assert raw[ord("日")].kun_bound == {"び", "か"}
    assert raw[ord("生")].kun_bound == frozenset()   # なま is also listed free


# --- the Jōyō table -------------------------------------------------------------------

PAGE1 = ("前書き\n"
         "\t 亜\t （亞）\t \t \t\t \t \t ア\t 亜流，亜麻\t\n"
         "行\t \t \t \t\t \t \t コウ\t 行進，行為\t\n"
         "\t \t \t ギョウ\t 行列，行政\t\n"
         "\t \t \t 　アン\t 行脚，行火\t\n"
         "\t \t \t いく\t 行く\t ⇔ 逝く\n"
         "弁\t\t\t辨\t \t \t\t \t \t ベン\t 弁償，花弁\t\n"
         "\t \t\t\t瓣\t \t \t\n"
         "異\t \t \t \t\t \t \t イ\t 異論\t\n"
         "コウ\n")
PAGE2 = ("\t \t \t こと\t 異なる\t\n"
         "餅\t［餅］\t \t \t\t \t \t ヘイ\t 煎餅\t ［餅］＝許容字体，\n"
         "\t \t （餠）\t もち\t 餅屋\t\n"
         "腕\t \t \t \t\t \t \t ワン\t 腕力\t\n")
PAGE3 = "付表\n\t あす\t 明日\t\n"


def test_parse_pages():
    t = parse_pages(["序\n\t 漢字\t 音訓\n", PAGE1, PAGE2, PAGE3])
    assert list(t) == ["亜", "行", "弁", "異", "餅", "腕"]
    assert t["行"] == [JoyoReading("コウ", False), JoyoReading("ギョウ", False),
                      JoyoReading("アン", True), JoyoReading("いく", False)]
    assert t["弁"] == [JoyoReading("ベン", False)]          # old forms are not kanji entries
    assert t["異"] == [JoyoReading("イ", False), JoyoReading("こと", False)]  # across pages
    assert t["餅"] == [JoyoReading("ヘイ", False), JoyoReading("もち", False)]
    assert "明" not in t and t["腕"] == [JoyoReading("ワン", False)]


def test_classify():
    entry = [JoyoReading("コウ", False), JoyoReading("アン", True), JoyoReading("いく", False),
             JoyoReading("こと", False)]
    assert classify("い.く", entry) == "joyo"
    assert classify("アン", entry) == "special"
    assert classify("ゆ.く", entry) == "other"
    assert classify("こと.なる", entry) == "joyo"    # the table prints the stem (異: こと, 異なる)
    assert classify("コウ", None) == "other"


@pytest.mark.skipif(not paths.JOYO_PDF.exists(), reason="run `tangocho-pack fetch --readings`")
def test_real_joyo_table():
    t = joyo_table.load_pdf(paths.JOYO_PDF)    # checks 2,136 kanji / 4,388 readings itself
    assert sum(1 for v in t.values() for r in v if r.indented) == 178
    assert [r.reading for r in t["九"]] == ["キュウ", "ク", "ここの", "ここのつ"]
    assert [r.reading for r in t["弟"] if r.indented] == ["ダイ", "デ"]


# --- the CLI ---------------------------------------------------------------------------

class _Stop(Exception):
    pass


KD2 = """<?xml version="1.0" encoding="UTF-8"?><kanjidic2>
<character><literal>九</literal><codepoint><cp_value cp_type="ucs">4e5d</cp_value></codepoint>
<misc><grade>1</grade><stroke_count>2</stroke_count><freq>55</freq><jlpt>4</jlpt></misc>
<reading_meaning><rmgroup><reading r_type="ja_on">キュウ</reading><reading r_type="ja_on">ク</reading>
<reading r_type="ja_kun">ここの</reading><reading r_type="ja_kun">ここの.つ</reading>
<meaning>nine</meaning></rmgroup></reading_meaning></character></kanjidic2>"""


@pytest.fixture
def fake_inputs(monkeypatch, tmp_path):
    kd = tmp_path / "kanjidic2.xml"
    kd.write_text(KD2, encoding="utf-8")
    levels = tmp_path / "kanji.json"
    levels.write_text(json.dumps({"九": {"jlpt_new": 5}}), encoding="utf-8")
    files = {"KANJIDIC_GZ": kd, "KANJI_JSON": levels,
             "JMDICT_GZ": (tmp_path / "JMdict_e", JMDICT.encode()),
             "JMDICT_FURIGANA": (tmp_path / "JmdictFurigana.txt", FURIGANA.encode()),
             "WORDFREQ_JA": (tmp_path / "large_ja.msgpack",
                             msgpack([{"format": "cB"}, [], ["九日"], ["九つ"]])),
             "TATOEBA_JPN": (tmp_path / "jpn_indices.csv", "1\t2\t九つ\n".encode()),
             "JOYO_PDF": (tmp_path / "joyo.pdf", b"%PDF")}
    for name, v in files.items():
        if isinstance(v, tuple):
            v[0].write_bytes(v[1])
            v = v[0]
        monkeypatch.setattr(paths, name, v)
    monkeypatch.setattr(joyo_table, "load_pdf", lambda p: {
        "九": [JoyoReading(r, False) for r in ("キュウ", "ク", "ここの", "ここのつ")]})
    seen = {}

    def fake(cards, *a, **kw):
        seen["cards"] = cards
        raise _Stop()

    monkeypatch.setattr(build, "build_pack", fake)
    return seen


def test_cli_common_readings_reorders(fake_inputs, tmp_path, capsys):
    with pytest.raises(_Stop):
        cli.main(["build", "--jlpt", "5", "--common-readings", "-o", str(tmp_path / "x.tgc")])
    (c,) = fake_inputs["cards"]
    assert c.kunyomi == ["ここの.つ", "ここの"] and c.onyomi == ["キュウ", "ク"]
    assert "first kun'yomi for 1 of 1 cards" in capsys.readouterr().out


def test_cli_default_keeps_kanjidic2_order(fake_inputs, tmp_path):
    with pytest.raises(_Stop):
        cli.main(["build", "--jlpt", "5", "-o", str(tmp_path / "x.tgc")])
    assert fake_inputs["cards"][0].kunyomi == ["ここの", "ここの.つ"]


def test_cli_common_readings_needs_fetch(fake_inputs, monkeypatch, tmp_path, capsys):
    monkeypatch.setattr(paths, "WORDFREQ_JA", tmp_path / "missing.msgpack.gz")
    rc = cli.main(["build", "--jlpt", "5", "--common-readings", "-o", str(tmp_path / "x.tgc")])
    assert rc == 2
    assert "fetch --readings" in capsys.readouterr().err
    assert "cards" not in fake_inputs


def test_cli_common_readings_not_for_wanikani(tmp_path, capsys):
    rc = cli.main(["build", "--wanikani-token", "x", "--common-readings",
                   "-o", str(tmp_path / "x.tgc")])
    assert rc == 2
    assert "WaniKani" in capsys.readouterr().err


# --- the real data (skipped without `fetch --readings`) ----------------------------------

REAL = [paths.KANJIDIC_GZ, paths.JMDICT_GZ, paths.JMDICT_FURIGANA, paths.WORDFREQ_JA,
        paths.TATOEBA_JPN, paths.JOYO_PDF]


@pytest.mark.skipif(not all(p.exists() for p in REAL), reason="run `tangocho-pack fetch --readings`")
def test_real_data_sanity():
    from tangocho_pack.readings import compute_scores

    raw = [k for k in parse_kanjidic(paths.KANJIDIC_GZ) if chr(k.codepoint) in "九食下上日入弟木"]
    scores = compute_scores(raw, paths.JMDICT_GZ, paths.JMDICT_FURIGANA, paths.WORDFREQ_JA,
                            paths.TATOEBA_JPN)
    out, _ = reorder_raw(raw, scores, joyo_table.load_pdf(paths.JOYO_PDF))
    first = {chr(k.codepoint): (k.onyomi[:1], k.kunyomi[:1]) for k in out}
    assert first["九"][1] == ["ここの.つ"]
    assert first["食"][1] == ["た.べる"]
    assert first["入"][1] == ["はい.る"]
    assert first["下"][1] == ["した"] and first["上"][1] == ["うえ"] and first["日"][1] == ["ひ"]
    assert first["弟"][0] == ["テイ"] and first["木"][0] == ["モク"]
    assert not math.isnan(scores.of(ord("九"), "キュウ"))
