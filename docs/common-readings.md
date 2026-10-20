# Common readings

Status (2026-10-20): researched, decided by the maintainer (below) and
implemented on the branch `feature/common-readings` as
`tangocho-pack build --common-readings`. `decks.yml` is prepared to use it for
the published decks but has not been run: the maintainer reviews the N5
changes first. Without the flag the builder's output is byte-identical to
before.

## Problem

A reading question asks the **first** reading of one kind (on'yomi or
kun'yomi) of the card. The builder copied KANJIDIC2's reading order, which is
not by frequency: 九's first kun'yomi is ここの (used in 九日 and 九重) rather than
ここの.つ, 食's is く.う rather than た.べる, 入's い.る rather than はい.る, and 重's
え rather than おも.い. And for 465 Jōyō kanji no kun'yomi at all is taught
(気 いき, 百 もも, 肉 しし, 部 べ), yet the device asks one every other time.

The invariant must keep holding: **no option may be another valid reading or
meaning of the target**. Rare readings of a kind that is asked therefore stay
in the card (they keep distractors out); they only stop being the asked one.

## How readings flow

**Builder** (`tools/pack-builder`):

- `kanjidic.parse_kanjidic` takes every `ja_on` (katakana) and `ja_kun`
  (hiragana) reading in KANJIDIC2's order, drops the `-` prefix/suffix marks
  (`-び` → `び`, remembering which kun'yomi were only listed that way), keeps
  the `.` okurigana mark and removes duplicates and empty items. Nanori are
  not read.
- The WaniKani importer (a user's own token) puts WaniKani's *primary* reading
  first. Those packs already ask the primary reading; `--common-readings`
  refuses to run on them.
- With `--common-readings`, `readings.reorder_raw` orders each kind and may
  empty one (below) before the cards are made.
- `pack.card_strings` joins each kind with `、`; an empty kind is the empty
  string, stored once in the string pool like any other string.
- `distractors.reading_distractors` ranks other cards by their **first**
  reading of the same kind (one or two kana apart), and drops a card whose first
  reading of either kind is **any** reading of the target, including the
  readings of a kind that `--common-readings` emptied.

**Pack** (docs/pack-format.md, "Readings"): within each kind the most useful
reading comes first, the device asks the first one, every listed reading is a
valid answer, and a kind may be empty (an offset to `""`, never 0).

**Firmware** (`quiz/Quiz.cpp`, `main.cpp`, `srs/Queue.cpp`):

- `main.cpp` alternates the asked kind, and `buildQuestion` falls back to the
  other kind when the card lacks the asked one (so an empty kind is never
  asked); `Queue` offers Reading mode when the card has either kind.
- The correct option is the **first** reading of the kind, `.` removed.
- A distractor shows its own card's first reading of the kind; the strict pass
  skips cards without that kind, and `tryAdd` rejects a reading that is
  **any** reading of that kind of the target.
- The mistake detail card shows every reading in pack order and skips an empty
  kind.

Reordering inside a kind therefore changes the asked reading with no format or
firmware change, and an empty kind uses the existing fallback. The firmware
side of empty kinds (including a card with neither kind, and the relaxed pass
checking both of the target's lists) is on `fix/quiz-relaxed-distractor`.

## Sources compared

Packs are redistributed under CC BY-SA 4.0 in the public `decks-v2` release,
so a source must allow that: public domain, CC BY or CC BY-SA. Coverage is for
the 2,383 kanji of JLPT N5-N1 ∪ Jōyō.

| source | what it gives | licence | coverage | effort | verdict |
|---|---|---|---|---|---|
| KANJIDIC2 `r_status`, `on_type` | would mark Jōyō readings and on'yomi type | CC BY-SA 4.0 | **none**: the DTD says "not currently used" and the 2026-289 file has 0 occurrences (checked) | — | no signal. KANJIDIC2's `freq` is per kanji, not per reading |
| **Jōyō table** (常用漢字表, Cabinet notice No. 2, 2010) | the 4,388 approved readings, 178 of them indented (１字下げ: "特別なものか，又は用法のごく狭いもの", special or of very narrow use: 行 アン, 弟 ダイ/デ, 何 なん) | public domain in Japan: Copyright Act art. 13(ii) excludes notices (告示) of national government bodies. The Agency's site also applies the 公共データ利用規約 1.0 (compatible with CC BY 4.0) | the 2,136 Jōyō kanji; no frequency | done: the official PDF parsed with pypdf (`joyo_table.py`) | **used**: approved readings first, and a Jōyō kanji's kind with no approved reading is not asked |
| joyodb (machine-readable Jōyō table) | readings + "uncommon" marks | **no licence** | Jōyō | — | not usable (and not needed) |
| kanjium `reg_on`/`reg_kun` | Jōyō readings, `*` = less common | CC BY-SA 4.0 | Jōyō | small | not needed: a secondary transcription that adds derived forms (願 ねがわしい), lacks some indentation (弟 ダイ, prefecture readings) and has extras (日 ニ) |
| **JMdict** priority tags | per word form and reading: `nfXX` (500-word bands of a Mainichi newspaper list), `news/ichi/spec/gai` 1/2 | CC BY-SA 4.0 (EDRDG) | all words | small | **used** as the word list and as a prior. Too coarse alone: verbs rarely get `nf` (行く, 生まれる have only `ichi1`), and a one-kanji form's tags are ambiguous (下 has `nf07` on もと) |
| **JmdictFurigana** | each JMdict word split into per-kanji readings (学校 = がっ + こう) | "the same licence as JMDict (CC BY-SA)" (README); code MIT | 236k word/reading pairs (release 2026-09-25); monthly releases | small | **used**: 98.9 % of the per-kanji parts match a KANJIDIC2 reading |
| **wordfreq** (Japanese "large" list) | how often each written form occurs (Wikipedia, subtitles, web, Twitter, Reddit), MeCab/IPAdic tokens, no readings | data CC BY-SA 4.0, code Apache-2.0 | 215k tokens; frozen snapshot (~2021, no more updates) | small (a 60-line MessagePack reader, no dependency) | **used** for magnitudes, through each word's forms and inflection stems |
| **Tatoeba** Japanese indices (Tanaka Corpus "B lines") | the JMdict headword and, for homographs, its reading in ~150k sentences: 上(うえ) 608 vs 上(じょう) 62; 下(した) 188 vs 下(もと) 6 | CC BY 2.0 FR (Tatoeba exports; the corpus moved from public domain to CC BY in 2009) | small but enough for common homographs; weekly export | small | **used** to split a form's count among its readings |
| **JLPT vocabulary lists** (Jonathan Waller, tanos.co.uk) | 7,270 N5-N1 words with their readings (the kanji-form decks): which readings learners meet first | CC BY ("Use my data!", below) | N5 499, N4 491, N3 1,578, N2 1,519, N1 3,183 words | small (stdlib `sqlite3`) | **used** as the learner signal |
| BCCWJ frequency lists (NINJAL) | lemma **with reading** frequencies from a balanced 104M-word corpus: the ideal data | "研究、教育目的であれば無償で自由に利用できる … **再配布は不可。商業使用（営利目的での利用）は要相談**" (manual §6); 編集著作権 NINJAL | everything | — | **not usable**: research/education only, no redistribution |
| jiten.moe frequency lists | word frequencies from 17,884 media titles | "All frequency lists are licensed under CC BY-SA 4.0" | large | unknown | **not used** (maintainer, 2026-10-20): readings per entry undocumented, the download seems to need an account, the site asks not to download systematically |
| kanjium `novels_freq.txt` / `wikipedia_freq.txt` | word frequencies | kanjium says CC BY-SA 4.0, but the lists come from a forum post with no licence | — | — | not usable: unclear provenance, lemmatisation errors (要る ranks first) |
| WaniKani primary readings, jpdb.io, kanji-data `wk_*` fields | primary / frequent readings | © Tofugu (API terms: no redistribution); jpdb publishes no licence for its data; `wk_*` are WaniKani-derived | — | — | **not usable** for redistributed packs |

URLs: KANJIDIC2/JMdict licence https://www.edrdg.org/edrdg/licence.html ·
Jōyō table https://www.bunka.go.jp/kokugo_nihongo/sisaku/joho/joho/kijun/naikaku/kanji/index.html
(PDF `joyokanjihyo_20101130.pdf`), site terms https://www.bunka.go.jp/bunkacho_homepage/index.html
→ https://www.mext.go.jp/b_menu/1351168.htm · Copyright Act art. 13
https://www.japaneselawtranslation.go.jp/en/laws/view/4207 · joyodb
https://github.com/melissaboiko/joyodb · kanjium https://github.com/mifunetoshiro/kanjium ·
JmdictFurigana https://github.com/Doublevil/JmdictFurigana · wordfreq
https://github.com/rspeer/wordfreq (README "License") · Tatoeba
https://tatoeba.org/en/downloads, https://www.edrdg.org/wiki/Tanaka_Corpus.html,
B-line format https://www.edrdg.org/wiki/Sentence-Dictionary_Linking.html ·
tanos.co.uk "Use my data!" https://web.archive.org/web/20250117052055/https://www.tanos.co.uk/jlpt/sharing/ ·
BCCWJ https://clrd.ninjal.ac.jp/bccwj/freq-list.html (manual
`BCCWJ_frequencylist_manual_ver1_0.pdf`) · jiten.moe https://jiten.moe/other,
https://jiten.moe/guides/deck-licensing, https://www.jiten.moe/guides/api-and-scraping.

### The JLPT vocabulary lists: source and licence chain

- **Author and licence.** Jonathan Waller's JLPT Resources (tanos.co.uk). Its
  "Use my data!" page (last updated 2011-10-12; the site no longer resolves,
  read from the Internet Archive copy above): "Everything on this site (that
  I'm not selling), is licenced under Creative Commons "BY". Basically this
  means... use anything here however you like (commercial or non-commercial),
  but credit my site." The vocabulary lists were free downloads, not sold, so
  the CC BY grant covers them, as it covers the kanji levels the builder already
  uses (docs/data-sources.md). CC BY material can go into a CC BY-SA pack with
  the credit.
- **Mirror.** The Internet Archive's captures of the original download files,
  `http://www.tanos.co.uk/jlpt/jlpt{N}/vocab/n{N}-vocab-kanji-hiragana.anki`,
  from 2011-07-22. A Wayback `id_` URL returns the archived bytes unchanged.
  `fetch.py` pins each file by capture timestamp and checks its SHA256. The
  2025 captures of the N1-N3 files have the same digests as the 2011 ones, so
  the content did not change in between. The original files were chosen over
  re-parsed copies on GitHub (e.g. Bluskyo/JLPT_Vocabulary, parsed from the
  PDFs), which add a transformation and a licence of their own to check.
- **Format.** Anki 1.x decks (SQLite): a `Front` (the word in kanji) and a
  `Back` (its reading) per fact. The `kanji-hiragana` decks hold only the words
  written with kanji, which are the ones that matter here. A few entries list
  alternatives (`川/河`, `しち / なな`, `お・金持ち`) and are expanded; a few hold
  grammar notes instead of a reading (`（感）`) and are skipped; one typo (N5
  `明い` for 明るい, reading あかるい) is still attributed, because a one-kanji
  word with a kana ending is aligned without JmdictFurigana. 7,224 (word,
  reading) pairs result.

## Decisions (maintainer, 2026-10-20)

1. **Jōyō-approved readings first**: kept as the first ordering tier.
2. **No pack-format extension** (no "do not ask" flags). Instead a kind with
   nothing worth asking is **left empty** and the device asks the other kind
   through its existing fallback. For a Jōyō kanji, a kind is worth asking when
   at least one of its readings is approved by the table; for another kanji,
   when at least one is used by some word. **Never both kinds**: if both fail,
   the kind with the higher total score stays; a card's only kind always stays.
   A kind that stays keeps all its readings, the unused ones last.
3. **Learner signal**: yes, the JLPT level of the words using a reading. Source
   and key below.
4. **Adopt for the published decks**, prepared on the branch (`decks.yml`,
   release-note attribution, data-sources.md, device credits); `decks.yml` is
   not run and nothing is pushed until the maintainer has reviewed the N5 list.
5. **Spec wording** in docs/pack-format.md: readings of each kind most useful
   first, the device asks the first, every listed reading is a valid answer, a
   kind may be empty.
6. **pypdf**: kept as a builder dependency.
7. **jiten.moe**: not used.
8. **Common non-approved readings** (等 など, 経 た.つ, 被 かぶ.る): later, a
   roadmap follow-up.

## What `--common-readings` does (`tangocho_pack/readings.py`)

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
3. **Learner weight.** A word on a JLPT list counts its frequency times 16
   (N5), 8 (N4), 4 (N3), 2 (N2) or 1.5 (N1); other words ×1.
4. **Per kanji.** JmdictFurigana gives each kanji's part of the reading (a
   one-kanji word with a kana ending it lacks is aligned by removing the
   ending). It is matched to the kanji's KANJIDIC2 readings, best match first:
   exact (with the okurigana that follows: 生きる → い.きる, not い.ける),
   okurigana in the furigana (生花 いけ → い.ける), a stem form (生き物 → い.きる),
   then with sequential voicing or gemination (人々 びと → ひと, 学校 がっ → ガク,
   一本 ぽん → ホン). Ties split the weight. Jukujikun (今日 きょう) have no
   per-kanji part. The same matching gives each reading its **level**: the
   easiest JLPT list level of a list word that uses it.
5. **Order.** Per kind, by: the tier (0 = approved by the Jōyō table, 1 =
   indented or not in the table; one tier for a non-Jōyō kanji); the level
   (N5 first; none last); the learner-weighted score; KANJIDIC2's order (so
   unused readings keep their old order, last). **Bound forms** have no level
   and count a quarter of their score: readings KANJIDIC2 only lists as a
   prefix or suffix (日 -び), a bare stem next to the same stem with okurigana
   (九 ここの / ここの.つ, 重 おも / おも.い) and a voiced or geminated variant of
   another reading (殻 がら / から).
6. **Empty kinds.** Decision 2. The emptied kind's readings ride along in the
   builder (`Card.valid_onyomi` / `valid_kunyomi`, not stored in the pack) so
   the precomputed reading distractors stay away from them too.

### Choosing the learner key

Two keys were compared on ten cases: four the frequency order got wrong (行
い.く, 出 で.る, 明 あか.るい, 生 い.きる or う.まれる) and six it got right and must
keep (九 ここの.つ, 食 た.べる, 入 はい.る, 四 よん, 一 ひと.つ, 大 おお.きい):

| key (after the Jōyō tier) | passes | misses |
|---|---:|---|
| frequency only | 6/10 | 行 おこな.う, 出 だ.す, 明 あき.らか, 生 なま |
| **lexicographic**: best level, then frequency | 9/10 | 出 だ.す (出る and 出す are both N5, and the compound verbs in 〜出す make だ.す more frequent) |
| **blended**: frequency × level multiplier, multipliers from 2..1.05 up to 100..1.5 (N5..N1) | 7/10 to 9/10 | always 明 あき.らか: its N5 evidence is the list's typo 明い, not a JMdict word, so no multiplier reaches it; 行 and 出 only with steep multipliers |
| **level, then the learner-weighted frequency** (chosen) | 10/10 | — |

The chosen key is the lexicographic one with the blend as its tie-break: the
easiest level a reading is met at decides, and among readings met at the same
level, the one in more and easier list words (出る, 出口, 出かける against 出す and
its compounds). The multipliers barely matter: 16/8/4/2/1.5 and 8/6/4/3/2 give
the same first reading for all 2,383 kanji. Against the plain lexicographic
key it changes 13 first readings, all toward the textbook one (出 で.る, 黒
くろ.い, 青 あお.い, 消 き.える, 落 お.ちる, 着 き.る, 三 みっ.つ, 間 あいだ…).

## Results

`uv run python scripts/common_readings_report.py` (after `fetch --readings`).
The set is JLPT N5-N1 ∪ Jōyō: 2,383 kanji. 507,445 per-kanji furigana parts
matched a KANJIDIC2 reading, 5,794 (1.1 %) did not (irregular readings, names).

**First reading changes** (each column adds to the previous; the last counts
only the kinds that stay):

| | JMdict priorities | + wordfreq | + Tatoeba | + Jōyō approved first | + JLPT lists | + empty kinds |
|---|---:|---:|---:|---:|---:|---:|
| first on'yomi changes (of 2,372) | 52 | 68 | 65 | 47 | 53 | 52 |
| first kun'yomi changes (of 1,988) | 370 | 312 | 311 | 267 | 257 | 189 |

**Changes and empty kinds by JLPT level** (no level: Jōyō kanji outside the
JLPT lists):

| | N5 | N4 | N3 | N2 | N1 | no level | all |
|---|---:|---:|---:|---:|---:|---:|---:|
| kanji | 79 | 166 | 367 | 367 | 1,232 | 172 | 2,383 |
| first on'yomi changed | 2 | 1 | 9 | 4 | 27 | 9 | 52 |
| first kun'yomi changed | 17 | 20 | 49 | 26 | 62 | 15 | 189 |
| on'yomi emptied | 0 | 0 | 3 | 5 | 52 | 25 | 85 |
| kun'yomi emptied | 5 | 13 | 48 | 60 | 353 | 71 | 550 |

**Both kinds would have gone** for 7 kanji; the kind with the higher score
stays: 茨, 岡, 埼 (Jōyō, no level), 唄 (N1) keep their kun'yomi (prefecture
readings, indented in the table); 彪, 晟, 燿 (N1, not Jōyō, no reading used by
any word) keep their on'yomi. **A card's only kind** had nothing worth asking
and stays for 5 kanji: 舜, 崚, 赳 (on, N1), 阜 (on), 栃 (kun).

**Unused readings** in the kinds that stay: 321 of 3,117 on'yomi and 256 of
3,164 kun'yomi are used by no word. They stay, last: valid answers, never
asked while a used reading exists.

**The asked reading against the Jōyō table** (Jōyō kanji only):

| kind | order | approved | special (indented) | not in the table | emptied |
|---|---|---:|---:|---:|---:|
| on | KANJIDIC2 | 2,049 | 1 | 80 | 0 |
| on | frequency only | 2,040 | 11 | 79 | 0 |
| on | `--common-readings` | **2,058** | 1 | 0 | 71 |
| kun | KANJIDIC2 | 1,290 | 4 | 483 | 0 |
| kun | frequency only | 1,258 | 7 | 512 | 0 |
| kun | `--common-readings` | **1,312** | 5 | 0 | 460 |

The six asked readings still special are the kinds that stay because the
other kind failed too or is absent: 茨 いばら, 岡 おか, 埼 さい, 栃 とち (prefecture
names), 唄 うた.う and 阜 フ, all indented in the table.

**Sanity table.** The asked reading before → after, and the order with each
reading's share of its kind's score (%) and the easiest JLPT list level of a
word using it. `*` indented in the Jōyō table, `†` not in it, `ᵇ` bound form.

| kanji | asked on | asked kun | on'yomi | kun'yomi |
|---|---|---|---|---|
| 九 | キュウ | ここの → **ここの.つ** | キュウ 88 N5 · ク 12 N5 | ここの.つ 81 N5 · ここのᵇ 19 N5 |
| 生 | セイ | い.きる → **う.まれる** | セイ 91 N5 · ショウ 9 N5 | う.まれる 37 N5 · い.きる 42 N4 · き 1 N3 · なま 9 N2 · は.える 2 N2 · い.ける 0 N2 · い.かす 1 N1 · は.やす 0 N1 · う.む 0 · お.う 0 · うま.れる 0 · う.まれ† 7 N3 · な.る† 0 N2 · う†ᵇ 1 · な.す† 0 · む.す† 0 · うまれ† 0 |
| 日 | ニチ | ひ | ニチ 93 N5 · ジツ 7 N3 | ひ 89 N4 · かᵇ 0 N5 · び†ᵇ 11 N5 |
| 下 | カ | した | カ 92 N5 · ゲ 8 N4 | した 84 N5 · さ.げる 6 N4 · さ.がる 2 N4 · お.りる 1 N4 · くだ.さる 5 N3 · お.ろす 1 N3 · くだ.る 1 N2 · くだ.す 0 N1 · もと 0 · しも 0 · くだ.り† 1 N3 |
| 上 | ジョウ | うえ | ジョウ 100 N4 · ショウ* 0 · シャン† 0 | うえ 81 N5 · あ.げる 13 N5 · あ.がる 3 N4 · のぼ.る 0 N2 · かみ 0 · のぼ.す 0 · のぼ.せる 0 · あ.がり† 1 N4 · のぼ.り† 0 N2 · うわ*ᵇ 1 N5 · たてまつ.る† 0 |
| 行 | コウ | い.く | コウ 96 N5 · ギョウ 4 N3 · アン* 0 | い.く 45 N5 · おこな.う 50 N4 · おこ.なう 0 N1 · ゆ.く 3 · い.き†ᵇ 1 N3 · ゆ.き†ᵇ 1 N2 · ゆき†ᵇ 0 · いき†ᵇ 0 |
| 出 | シュツ | で.る | シュツ 100 N3 · スイ* 0 | で.る 47 N5 · だ.す 39 N5 · い.でる† 0 N1 · で†ᵇ 14 N5 · い.だす† 0 |
| 明 | メイ | あ.かり → **あか.るい** | メイ 94 N4 · ミョウ 6 N3 · ミン† 0 | あか.るい 9 N5 · あか.るむ 0 N5 · あき.らか 66 N3 · あ.ける 10 N3 · あ.かり 3 N3 · あ.かす 4 N2 · あ.く 0 N2 · あ.くる 0 N2 · あか.らむ 0 · あ.け†ᵇ 7 N3 |
| 人 | ジン | ひと | ジン 54 N5 · ニン 46 N4 | ひと 100 N5 · と†ᵇ 0 · り†ᵇ 0 |
| 大 | ダイ | おお → **おお.きい** | ダイ 79 N5 · タイ 21 N5 | おお.きい 85 N5 · おおᵇ 14 N5 · おお.いにᵇ 1 N3 |
| 何 | カ | なに | カ 100 | なに 86 N5 · なん* 14 N5 |
| 今 | コン | いま | コン 100 N5 · キン 0 | いま 100 N5 |
| 重 | ジュウ | え → **おも.い** | ジュウ 80 N3 · チョウ 20 N3 | おも.い 84 N5 · かさ.ねる 8 N2 · かさ.なる 4 N2 · え 2 N1 · おも.なう† 0 N1 · おも†ᵇ 3 N2 · おも.り† 0 |
| 楽 | ガク | たの.しい | ガク 84 N5 · ラク 16 N3 · ゴウ† 0 | たの.しい 86 N5 · たの.しむ 14 N4 · この.む† 0 |
| 方 | ホウ | かた | ホウ 100 N4 | かた 93 N5 · がた†ᵇ 7 N5 |
| 間 | カン | あいだ | カン 92 N5 · ケン 8 N3 | あいだ 59 N4 · ま 41 N4 · あい† 0 |
| 食 | ショク | く.う → **た.べる** | ショク 99 N5 · ジキ* 1 | た.べる 94 N5 · く.う 6 N3 · く.らう 0 N1 · は.む† 0 |
| 入 | ニュウ | い.る → **はい.る** | ニュウ 100 N3 · ジュ† 0 | はい.る 75 N5 · い.れる 21 N5 · い.る 1 N3 · い.り†ᵇ 2 N5 · い.れ†ᵇ 1 N4 |
| 四 | シ | よ → **よん** | シ 100 N5 | よん 93 N5 · よっ.つ 5 N5 · よ.つ 0 N5 · よᵇ 2 |
| 一 | イチ | ひと → **ひと.つ** | イチ 84 N5 · イツ 16 N5 | ひと.つ 88 N5 · ひとᵇ 12 N5 |

**JLPT N5, every change** (79 cards; KANJIDIC2 → `--common-readings`):

- on'yomi (2): 右 ウ → ユウ, 木 ボク → モク.
- kun'yomi (17): 一 ひと → ひと.つ, 二 ふた → ふた.つ, 三 み → みっ.つ, 四 よ → よん,
  五 いつ → いつ.つ, 六 む → むっ.つ, 七 なな → なな.つ, 八 や → やっ.つ,
  九 ここの → ここの.つ, 大 おお → おお.きい, 白 しろ → しろ.い, 生 い.きる →
  う.まれる, 食 く.う → た.べる, 入 い.る → はい.る, 後 のち → あと, 天 あまつ → あめ,
  話 はな.す → はなし.
- kun'yomi emptied (5; the device asks their on'yomi): 気 (いき, き), 午 (うま),
  百 (もも), 毎 (ごと, ごと.に), 万 (よろず).

**Built decks.** Every default deck (N5-N1 and Jōyō, es and en, with stroke
order) builds with the flag, validates, and passes
`scripts/check_reading_distractors.py`: no card's precomputed reading
distractor shows a reading of the target, checked against KANJIDIC2's full
lists (so the emptied kinds too). That list gives the device fewer than the 3
wrong options a question needs for 0 % of the asked kinds in N5-N2, 0.7 % in N1
(13 of 1,788) and 0.5 % in the Jōyō deck (16 of 3,376); only then does the
device fill from a random walk over the pack. That walk takes cards with the
asked kind and checks them against the target's list of that kind, which is
complete, so emptied readings can only appear through the relaxed pass (a card
of the other kind), which needs a pack too small to fill four options. Sizes
are unchanged (N5 105 KB, Jōyō 3.0 MB); each build takes ~10 s longer.

## Integration

- **Builder**: `fetch --readings` (~37 MB into `cache/readings/`) and
  `build --common-readings`; `scripts/common_readings_report.py`,
  `scripts/check_reading_distractors.py`; tests in `tests/test_readings.py`.
  The pack validator rejects a reading list with an empty item.
- **Pack format**: the "Readings" paragraph of docs/pack-format.md (spec only,
  no layout change, no version bump).
- **`decks.yml`** (prepared, not run): `fetch --readings`, `--common-readings`
  on every build line, the distractor check after the pack checks, and the
  release notes' description and attribution.
- **Credits**: docs/data-sources.md; `firmware/src/Credits.h` names JMdict next
  to KANJIDIC2 (both EDRDG, shown on Settings › Device › About as before) and
  holds the other reading-order sources in `kReadings`, which the portal page
  footer shows. The About screen would need one more row in `main.cpp` to show
  `kReadings` (not done: `main.cpp` is outside this branch's scope).
- **Firmware**: nothing else needed here; empty kinds are handled on
  `fix/quiz-relaxed-distractor`.

## Follow-ups

- Let a few common readings the table does not approve be asked (等 など, 経
  た.つ, 被 かぶ.る), e.g. for learners outside the school curriculum
  (docs/roadmap.md).
- The About row for `kReadings` (`main.cpp`).
- Known limits: one JMdict entry with two readings (行く いく/ゆく) is split
  evenly; place names inside compounds add noise (三重 みえ to 重 え); wordfreq
  is frozen at ~2021; the tanos lists date from 2011 and have a few errors.
