# Common readings

Status: research and an opt-in builder flag (`build --common-readings`) on the
branch `feature/common-readings`, 2026-10-20. The default build output,
`decks.yml` and the pack format are unchanged. Decisions for the maintainer are
listed at the end.

## Problem

A reading question asks the **first** reading of one kind (on'yomi or
kun'yomi) of the card. The builder copies KANJIDIC2's reading order, which is
not by frequency: 九's first kun'yomi is ここの (used in 九日 and 九重) rather than
ここの.つ, 食's is く.う rather than た.べる, 入's い.る rather than はい.る, and 重's
え rather than おも.い. So the device sometimes asks a rare reading.

The invariant must keep holding: **no option may be another valid reading or
meaning of the target**. Rare readings therefore have to stay in the card (they
keep distractors out); they should only stop being the asked reading.

## How readings flow today

**Builder** (`tools/pack-builder`):

- `kanjidic.parse_kanjidic` takes every `ja_on` (katakana) and `ja_kun`
  (hiragana) reading in KANJIDIC2's order, drops the `-` prefix/suffix marks
  (`-び` → `び`), keeps the `.` okurigana mark and removes duplicates. Nanori
  are not read. Nothing is capped.
- The WaniKani importer (a user's own token) puts WaniKani's *primary* reading
  first, then the accepted ones. Those packs already ask the primary reading.
- `pack.card_strings` joins each kind with `、` into the card's `onyomi` and
  `kunyomi` strings. The pack spec (docs/pack-format.md) says what the strings
  hold, but nothing about their order (meanings are "most common first";
  readings have no such rule).
- `distractors.reading_distractors` ranks other cards by their **first**
  reading of the same kind (one or two kana apart), and drops a card whose first
  reading is **any** reading of the target (both kinds checked).

**Firmware** (`firmware/src/quiz/Quiz.cpp`, `main.cpp`):

- `main.cpp` alternates the asked kind (`gNextReadingIsKun`), and
  `buildQuestion` falls back to the other kind if the card lacks the asked one.
- The correct option is `firstReadingDisplay()` of that kind: the **first**
  reading, with the `.` removed. Only one reading is ever the answer.
- A distractor shows its own card's first reading of the kind. `tryAdd` rejects
  it if it equals **any** reading of that kind of the target
  (`containsReading`), so all readings count as valid, whatever their order.
- The mistake detail card shows all readings, in pack order.

So **reordering the readings inside each kind changes which one is asked,
without any format or firmware change**, and the invariant is unaffected
because the exclusion uses the whole list. (One small gap, unrelated to the
order: in `buildQuestion`'s relaxed pass, a distractor of the *other* kind is
only checked against the target's readings of the asked kind, so a katakana
option could be one of the target's on'yomi in a kun'yomi question. It only
happens when the strict pass cannot fill four options, i.e. in tiny packs.)

## Sources compared

Packs are redistributed under CC BY-SA 4.0 in the public `decks-v2` release,
so a source must allow that: public domain, CC BY or CC BY-SA. Coverage is for
the 2,383 kanji of JLPT N5-N1 ∪ Jōyō.

| source | what it gives | licence | coverage | effort | verdict |
|---|---|---|---|---|---|
| KANJIDIC2 `r_status`, `on_type` | would mark Jōyō readings and on'yomi type | CC BY-SA 4.0 | **none**: the DTD says "not currently used" and the 2026-289 file has 0 occurrences (checked) | — | no signal. KANJIDIC2's `freq` is per kanji, not per reading |
| **Jōyō table** (常用漢字表, Cabinet notice No. 2, 2010) | the 4,388 approved readings, 178 of them indented (１字下げ: "特別なものか，又は用法のごく狭いもの", special or of very narrow use: 行 アン, 弟 ダイ/デ, 何 なん) | public domain in Japan: Copyright Act art. 13(ii) excludes notices (告示) of national government bodies. The Agency's site also applies the 公共データ利用規約 1.0 (compatible with CC BY 4.0) | the 2,136 Jōyō kanji; no frequency | done: the official PDF parsed with pypdf (`joyo_table.py`) | **use** as a tier: approved readings first; and as the basis of a "do not ask" flag |
| joyodb (machine-readable Jōyō table) | readings + "uncommon" marks | **no licence** | Jōyō | — | not usable (and not needed) |
| kanjium `reg_on`/`reg_kun` | Jōyō readings, `*` = less common | CC BY-SA 4.0 | Jōyō | small | not needed: a secondary transcription that adds derived forms (願 ねがわしい), lacks some indentation (弟 ダイ, prefecture readings) and has extras (日 ニ) |
| **JMdict** priority tags | per word form and reading: `nfXX` (500-word bands of a Mainichi newspaper list), `news/ichi/spec/gai` 1/2 | CC BY-SA 4.0 (EDRDG) | all words | small | **use** as the word list and as a prior. Too coarse alone: verbs rarely get `nf` (行く, 生まれる have only `ichi1`), and a one-kanji form's tags are ambiguous (下 has `nf07` on もと) |
| **JmdictFurigana** | each JMdict word split into per-kanji readings (学校 = がっ + こう) | "the same licence as JMDict (CC BY-SA)" (README); code MIT | 236k word/reading pairs (release 2026-09-25); monthly releases | small | **use**: 98.9 % of the per-kanji parts match a KANJIDIC2 reading |
| **wordfreq** (Japanese "large" list) | how often each written form occurs (Wikipedia, subtitles, web, Twitter, Reddit), MeCab/IPAdic tokens, no readings | data CC BY-SA 4.0, code Apache-2.0 | 215k tokens; frozen snapshot (~2021, no more updates) | small (a 60-line MessagePack reader, no dependency) | **use** for magnitudes, through each word's forms and inflection stems |
| **Tatoeba** Japanese indices (Tanaka Corpus "B lines") | the JMdict headword and, for homographs, its reading in ~150k sentences: 上(うえ) 608 vs 上(じょう) 62; 下(した) 188 vs 下(もと) 6 | CC BY 2.0 FR (Tatoeba exports; the corpus moved from public domain to CC BY in 2009) | small but enough for common homographs; weekly export | small | **use** to split a form's count among its readings |
| BCCWJ frequency lists (NINJAL) | lemma **with reading** frequencies from a balanced 104M-word corpus: the ideal data | "研究、教育目的であれば無償で自由に利用できる … **再配布は不可。商業使用（営利目的での利用）は要相談**" (manual §6); 編集著作権 NINJAL | everything | — | **not usable**: research/education only, no redistribution |
| jiten.moe frequency lists | word frequencies from 17,884 media titles (anime, novels, VNs…) | "All frequency lists are licensed under CC BY-SA 4.0" | large | unknown | **not evaluated**: whether entries carry readings is not documented, the download seems to need an account, and the site asks not to download systematically. Worth a manual look (open question) |
| kanjium `novels_freq.txt` / `wikipedia_freq.txt` | word frequencies | kanjium says CC BY-SA 4.0, but the lists come from a forum post with no licence | — | — | not usable: unclear provenance, lemmatisation errors (要る ranks first) |
| WaniKani primary readings, jpdb.io, kanji-data `wk_*` fields | primary / frequent readings | © Tofugu (API terms: no redistribution); jpdb publishes no licence for its data; `wk_*` are WaniKani-derived | — | — | **not usable** for redistributed packs. A WaniKani pack built with the user's own token already asks the primary reading |

URLs: KANJIDIC2/JMdict licence https://www.edrdg.org/edrdg/licence.html ·
Jōyō table https://www.bunka.go.jp/kokugo_nihongo/sisaku/joho/joho/kijun/naikaku/kanji/index.html
(PDF `joyokanjihyo_20101130.pdf`), site terms https://www.bunka.go.jp/bunkacho_homepage/index.html
→ https://www.mext.go.jp/b_menu/1351168.htm · Copyright Act art. 13
https://www.japaneselawtranslation.go.jp/en/laws/view/4207 · joyodb
https://github.com/melissaboiko/joyodb · kanjium https://github.com/mifunetoshiro/kanjium ·
JmdictFurigana https://github.com/Doublevil/JmdictFurigana · wordfreq
https://github.com/rspeer/wordfreq (README "License") · Tatoeba
https://tatoeba.org/en/downloads, https://www.edrdg.org/wiki/Tanaka_Corpus.html,
B-line format https://www.edrdg.org/wiki/Sentence-Dictionary_Linking.html · BCCWJ
https://clrd.ninjal.ac.jp/bccwj/freq-list.html (manual
`BCCWJ_frequencylist_manual_ver1_0.pdf`) · jiten.moe
https://jiten.moe/other, https://jiten.moe/guides/deck-licensing,
https://www.jiten.moe/guides/api-and-scraping.

## Recommendation

**Score readings from JMdict + JmdictFurigana + wordfreq + Tatoeba, and put the
Jōyō table's approved readings first.** All four word sources are CC BY-SA 4.0
or CC BY (attribution only), so a pack built with them stays CC BY-SA 4.0, like
today's KANJIDIC2/KanjiVG packs; the Jōyō table is public domain. No single
open source has per-reading frequencies (BCCWJ does, but cannot be
redistributed), so the per-reading score is assembled: wordfreq says how common
a written word is, Tatoeba says which reading a homograph has, JMdict and
JmdictFurigana say which reading of which kanji each word uses. The Jōyō table
then keeps the asked reading among those taught in Japanese schools, even when
a special reading is frequent because of one word (弟 ダイ from 兄弟, 納 ナッ from
納得, 嫌 ゲン from 機嫌).

Ordering alone fixes most cases without touching the pack format. The rest,
kanji with **no** approved reading of a kind (465 Jōyō kanji for kun'yomi: 気
いき, 百 もも, 肉 しし, 部 べ…), needs a small format extension (below) so the
device asks the other kind for them.

## Method (`tangocho_pack/readings.py`)

1. **Words.** Every JMdict (kanji form, reading) pair, skipping forms or
   readings marked rare, search-only, outdated or irregular, `re_nokanji`, and
   readings restricted to another form. Prior weight: 1 / rank, where `nfXX` →
   500·XX − 250, `news1/ichi1/spec1/gai1` → 12,000, the `2` tags → 30,000, none
   → 100,000; a pair is as rare as the rarer of its form and reading (今日 こんち
   gets 100,000); ×0.1 if the first sense is "usually written in kana" (為る).
2. **Corpus weight.** For each pair, the wordfreq frequency of its form and,
   for verbs and i-adjectives, of its inflection stems as MeCab tokenises them
   (行く: 行か 行き 行く 行け 行こ 行っ; 生まれる: 生まれ). A token claimed by several
   pairs is shared among them in proportion to their prior; where Tatoeba has
   counts for the form, the share is the smoothed Tatoeba frequency of each
   reading (2 pseudo-counts of the prior). A one-character form (上, 下) takes a
   corpus share only with at least 5 Tatoeba sentences, otherwise just its
   prior: a lone kanji token is often a piece split off a longer word and its
   JMdict tags cannot tell readings apart. Plus 0.001 × prior, so words
   wordfreq lacks still count a little.
3. **Per kanji.** JmdictFurigana gives each kanji's part of the reading. It is
   matched to the kanji's KANJIDIC2 readings, best match first: exact (with
   the okurigana that follows: 生きる → い.きる, not い.ける), okurigana in the
   furigana (生花 いけ → い.ける), a stem form (生き物 → い.きる), then with
   sequential voicing or gemination (人々 びと → ひと, 学校 がっ → ガク, 一本 ぽん →
   ホン). Ties split the weight. Jukujikun (今日 きょう) have no per-kanji part
   and count for nothing.
4. **Order.** Per kind: tier first (0 = approved by the Jōyō table, 1 =
   indented or not in the table; one tier for non-Jōyō kanji), then the score,
   then KANJIDIC2's order (so unused readings stay in their old order, last).
   Bound forms count a quarter of their score: readings KANJIDIC2 only lists as
   a prefix or suffix (日 -び), a bare stem next to the same stem with okurigana
   (九 ここの / ここの.つ, 重 おも / おも.い), and a voiced or geminated variant of
   another reading (殻 がら / から).

Nothing is dropped: every card keeps exactly the same set of readings.

## Prototype results

`uv run python scripts/common_readings_report.py` (after `fetch --readings`;
16 s). The set is JLPT N5-N1 ∪ Jōyō: 2,383 kanji. 507,439 per-kanji furigana
parts matched a KANJIDIC2 reading, 5,792 (1.1 %) did not (irregular readings,
names).

**First reading changes** (each column adds to the previous):

| | JMdict priorities | + wordfreq | + Tatoeba | + Jōyō approved first (`--common-readings`) |
|---|---:|---:|---:|---:|
| first on'yomi changes (of 2,372) | 52 | 68 | 65 | **47** |
| first kun'yomi changes (of 1,988) | 370 | 312 | 311 | **267** |

**The asked reading against the Jōyō table** (Jōyō kanji only):

| kind | order | approved | special (indented) | not in the table |
|---|---|---:|---:|---:|
| on | KANJIDIC2 | 2,049 | 1 | 80 |
| on | frequency only | 2,040 | 11 | 79 |
| on | `--common-readings` | **2,058** | 1 | 71 |
| kun | KANJIDIC2 | 1,290 | 4 | 483 |
| kun | frequency only | 1,258 | 7 | 512 |
| kun | `--common-readings` | **1,312** | 5 | 460 |

Frequency alone would move the asked reading of 67 kanji kinds from an approved
reading to a special or unapproved one (弟 ダイ, 納 ナッ, 等 など, 経 た.つ, and
KANJIDIC2's noun forms such as 通 とお.り, 掛 か.け) and 26 the other way; with
the tier none move away from an approved reading, and 31 move to one (図 え →
はか.る, 歯 よわい → は, 粉 デシメートル → こな, 志 シリング → こころざ.す).

**Unused readings.** 376 of 3,235 on'yomi and 777 of 4,134 kun'yomi are used by
no word in the data. They stay in the card, after the used ones, so they still
count as valid answers but are never asked while a used reading exists. 33
kanji have no used on'yomi and 223 no used kun'yomi; they keep KANJIDIC2's
order (mostly rare N1 / non-Jōyō kanji).

**Sanity table.** The asked reading (first) before → after, and the order with
each reading's share of its kind's score in %. `*` indented in the Jōyō table,
`†` not in it, `ᵇ` bound form.

| kanji | asked on | asked kun | on'yomi | kun'yomi |
|---|---|---|---|---|
| 九 | キュウ | ここの → **ここの.つ** | キュウ 86 · ク 14 | ここの.つ 44 · ここのᵇ 56 |
| 生 | セイ | い.きる → **なま** | セイ 87 · ショウ 13 | なま 35 · い.きる 33 · う.まれる 10 · は.える 3 · い.かす 3 · う.む 2 · き 1 · い.ける 1 · は.やす 0 · お.う 0 · うま.れる 0 · う.まれ† 9 · う†ᵇ 3 · な.る† 0 · な.す† 0 · む.す† 0 · うまれ† 0 |
| 日 | ニチ | ひ | ニチ 80 · ジツ 20 | ひ 92 · かᵇ 1 · び†ᵇ 7 |
| 下 | カ | した | カ 82 · ゲ 18 | した 43 · くだ.さる 29 · さ.げる 7 · さ.がる 7 · お.ろす 3 · くだ.す 3 · もと 2 · くだ.る 2 · しも 1 · お.りる 1 · くだ.り† 2 |
| 上 | ジョウ | うえ | ジョウ 100 · ショウ* 0 · シャン† 0 | うえ 45 · あ.げる 30 · あ.がる 15 · かみ 1 · のぼ.る 1 · のぼ.す 0 · のぼ.せる 0 · あ.がり† 3 · うわ*ᵇ 3 · のぼ.り† 1 · たてまつ.る† 0 |
| 行 | コウ | い.く → **おこな.う** | コウ 87 · ギョウ 13 · アン* 0 | おこな.う 47 · ゆ.く 21 · い.く 21 · おこ.なう 1 · ゆ.き†ᵇ 6 · い.き†ᵇ 4 · ゆき†ᵇ 0 · いき†ᵇ 0 |
| 人 | ジン | ひと | ジン 55 · ニン 45 | ひと 100 · と†ᵇ 0 · り†ᵇ 0 |
| 大 | ダイ | おお → **おお.きい** | ダイ 72 · タイ 28 | おお.きい 37 · おおᵇ 61 · おお.いにᵇ 2 |
| 何 | カ | なに | カ 100 | なに 80 · なん* 20 |
| 今 | コン | いま | コン 100 · キン 0 | いま 100 |
| 明 | メイ | あ.かり → **あき.らか** | メイ 83 · ミョウ 17 · ミン† 0 | あき.らか 49 · あか.るい 18 · あ.ける 10 · あ.かす 9 · あ.かり 3 · あか.るむ 1 · あ.く 1 · あ.くる 0 · あか.らむ 0 · あ.け†ᵇ 9 |
| 重 | ジュウ | え → **おも.い** | ジュウ 83 · チョウ 17 | おも.い 33 · かさ.ねる 29 · かさ.なる 14 · え 11 · おも†ᵇ 14 · おも.り† 0 · おも.なう† 0 |
| 楽 | ガク | たの.しい | ガク 54 · ラク 46 · ゴウ† 0 | たの.しい 65 · たの.しむ 35 · この.む† 0 |
| 方 | ホウ | かた | ホウ 100 | かた 92 · がた†ᵇ 8 |
| 間 | カン | あいだ → **ま** | カン 81 · ケン 19 | ま 60 · あいだ 40 · あい† 0 |

Close calls: 生 なま 35 / い.きる 33; 行 おこな.う 47 against い.く and ゆ.く 21
each, because JMdict gives 行く's two readings the same priority and Tatoeba
does not tell them apart (one entry), so 行く's count is split in half.

**JLPT N5 deck** (79 cards): the first on'yomi changes for 3 cards, the first
kun'yomi for 21. All of them, KANJIDIC2 → `--common-readings`:
木 ボク → モク, 右 ウ → ユウ, 西 セイ → サイ; 一 ひと → ひと.つ, 二 ふた → ふた.つ,
三 み → み.つ, 四 よ → よん, 五 いつ → いつ.つ, 六 む → むっ.つ, 七 なな → なな.つ,
八 や → やっ.つ, 九 ここの → ここの.つ, 大 おお → おお.きい, 白 しろ → しろ.い,
入 い.る → はい.る, 食 く.う → た.べる, 後 のち → あと, 天 あまつ → あめ,
話 はな.す → はなし, 間 あいだ → ま, 出 で.る → だ.す, 生 い.きる → なま,
行 い.く → おこな.う, 気 いき → き. The reading-distractor invariant check over
the built pack finds no violation, and every card keeps the same set of
readings.

## Proposed integration

### Builder (on this branch)

- `tangocho-pack fetch --readings` downloads into `cache/readings/`: JMdict
  (`JMdict_e.gz`), JmdictFurigana (`JmdictFurigana.txt`, pinned release),
  wordfreq's `large_ja.msgpack.gz` (pinned tag), Tatoeba's
  `jpn_indices.tar.bz2` and the Jōyō table PDF (~30 MB in all).
- `tangocho-pack build ... --common-readings` reorders each kanji's on'yomi and
  kun'yomi as above before the cards are made; it prints how many cards change
  their first reading. It adds ~8 s to a build (JMdict and the PDF are read
  every time). It is refused with `--wanikani-token`. Without the flag the
  output is unchanged.
- `scripts/common_readings_report.py` prints the tables in this document.
- New dependency: `pypdf` (BSD-3-Clause, pure Python) to read the PDF.
- Tests: `tests/test_readings.py` (hand-written miniatures of every input
  format, plus checks against the real data when the cache has it).

To adopt it for the published decks: add `--common-readings` to the
`decks.yml` build lines and `fetch --readings` to its fetch step, and extend the
attribution (below). Not done here.

### Pack format (proposal only, not implemented)

The order needs no format change, but the spec should say what the device
relies on. Proposed wording for docs/pack-format.md, card `onyomi`/`kunyomi`:

> Readings of each kind are ordered most useful first: the device asks the
> first one. Every listed reading counts as a valid answer.

For kinds with nothing worth asking, two bits of the card's `flags` byte:

| bit | meaning |
|---|---|
| 0 | meaning fell back to English (as today) |
| 1 | do not ask the on'yomi: none is worth asking (they still count as valid answers) |
| 2 | do not ask the kun'yomi: likewise |

0 means "ask", so older packs keep today's behaviour and no version bump is
needed (the firmware stores `flags` and reads no bit of it today). Builder
rule, proposed: for a Jōyō kanji, set the bit when none of the kind's readings
is approved by the table; for a non-Jōyō kanji, when no reading of the kind is
used by any word (score 0). Counts in N5-N1 ∪ Jōyō (by new JLPT level):

| | N5 | N4 | N3 | N2 | N1 | no level (Jōyō) |
|---|---:|---:|---:|---:|---:|---:|
| kun'yomi bit (Jōyō rule) | 5 | 13 | 48 | 60 | 264 | 75 |
| on'yomi bit (Jōyō rule) | 0 | 0 | 3 | 5 | 38 | 26 |
| neither kind askable | 0 | 0 | 0 | 0 | 1 | 5 |

The N5 ones: 気 (いき, き), 午 (うま), 百 (もも), 毎 (ごと, ごと.に), 万 (よろず).
Of the 247 non-Jōyō N1 kanji, 107 have a kind no word uses.

### Firmware (proposal, with the flags)

- `quiz/Quiz.cpp`: an `askable(card, kind)` = has the kind and its bit is
  clear; `buildQuestion` picks the asked kind among the askable ones (falling
  back as today) and returns false for Reading mode when neither is askable;
  `containsReading` is unchanged (all readings stay valid). Optionally prefer
  distractor cards whose kind is askable, since a non-askable card's first
  reading is a rare one.
- `srs/Queue.cpp`: `can[Reading]` true only if some kind is askable.
- The detail card keeps showing every reading (could later dim unasked ones).
- Unrelated small fix noticed on the way: check the relaxed-pass distractor
  against **both** of the target's lists.

### Attribution, if adopted for the published decks

A pack built with `--common-readings` contains no word data, only an order of
KANJIDIC2's readings derived from these sources, but crediting them is the
safe reading of their licences:

> Reading order from JMdict (EDRDG, CC BY-SA 4.0), JmdictFurigana (CC BY-SA),
> wordfreq (Robyn Speer, CC BY-SA 4.0, with data from OpenSubtitles), the
> Tatoeba Project's sentence indices (CC BY 2.0 FR) and the Jōyō kanji table
> (Agency for Cultural Affairs, 2010).

It is in docs/data-sources.md (combined attribution, for packs built with the
flag); it would also go into the decks release notes and, if shown on the
device, `firmware/src/Credits.h`.

## Open questions for the maintainer

1. **Adopt `--common-readings` for the decks?** It changes the asked kun'yomi
   of 13 % of the kanji that have one (267 of 1,988; N5: 21 of 79 cards) and
   the asked on'yomi of 2 % (47 of 2,372). If yes: `decks.yml` and the
   attribution as above.
2. **Approved readings first?** The tier is what keeps 弟 テイ and 等 ひと.しい
   instead of the more frequent ダイ and など. The alternative is pure frequency
   (closer to what a reader meets, further from what a school teaches).
3. **The flags bits 1-2** (format extension above) and the firmware change, so
   kanji like 気, 百, 肉 stop asking a kun'yomi nobody uses. Which rule for
   non-Jōyō kanji?
4. **Spec wording** that readings are "most useful first, the first is asked".
5. **The `pypdf` dependency**, or a different way to read the Jōyō table
   (e.g. committing a derived TSV: public domain, but CLAUDE.md keeps
   downloaded data out of the repo).
6. **Known limits**: one JMdict entry with two readings (行く いく/ゆく) is split
   evenly; place names inside compounds add noise (三重 みえ to 重 え, 九重 to
   ここの); wordfreq is frozen at ~2021. Is jiten.moe (CC BY-SA 4.0, a large
   media corpus) worth a manual check for per-reading counts?
7. **Should a few common non-approved readings be askable anyway** (等 など,
   経 た.つ, 被 かぶ.る), e.g. for non-school learners?
