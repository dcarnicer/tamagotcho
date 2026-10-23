# Example words and their glosses

Status (2026-10-23): measured for decision 9 of docs/learning-mode.md
(§3.11: example words on the new-kanji intro card). The numbers come
from `tools/pack-builder/scripts/example_words_report.py` and, for the
licence-compatible alternatives to JMdict's Spanish glosses ("Alternatives",
which revises the recommendation), `scripts/example_words_alternatives.py`.
**Since implemented on the data side** (branch `feature/example-words`): the
pick below moved into the builder (`tamagotcho_pack/words.py`, `build
--example-words`, which writes pack format v3; the scripts import it), the
Spanish table is `data/example_glosses_es.tsv` (filled by `sync-glosses`),
and the firmware reads the words (`PackReader::exampleWords()`); no
published deck has them yet (docs/pack-format.md, "Rollout of v3"). One
change from the measurement: the table's `skip` rows remove a word in every
language, and the next candidate is taken.

## Question

Decision 9 postponed example words until we know whether JMdict can supply
them in Spanish. Option (b) puts one or two common words per kanji in the pack
with their reading and a gloss in the deck's language; option (c) puts the word
and its reading only. This document measures, for the published decks (JLPT
N5-N1 and the Jōyō deck, `decks.yml`):

1. how many kanji get a word for each asked reading;
2. how many of those words have a Spanish gloss in the multilingual JMdict,
   and a short one, against English;
3. whether the Spanish glosses look right to a learner (a spot check);
4. what (b) and (c) would add to each pack.

It also found a licence problem that decides more than the numbers do
("Licence", below).

## Method

**The asked readings.** A card's asked reading of a kind is its first one
after `--common-readings` (docs/common-readings.md); a kind may be empty. The
script computes them with the builder's own code (`readings.compute_scores`,
then `reorder_raw(..., drop=True)`), and checks them against the published
`decks-v2` packs (rebuilt 2026-10-22): no mismatch over the 4,347 cards of
the Spanish packs.

**The candidate words.** Every JMdict (kanji form, reading) pair that
`readings.parse_jmdict` yields (rare, irregular and search-only forms and
readings are already skipped), weighted the way `--common-readings` weighs
words: the wordfreq frequency of the form and its inflection stems, shared
among readings by Tatoeba and the JMdict priority tags, times the JLPT list
multiplier (16, 8, 4, 2, 1.5 for N5..N1). A word uses a reading when
JmdictFurigana's part for that kanji matches it, by the same attribution that
scores the readings (`match_level`), so sequential voicing and gemination
count (学校 がっ → ガク, 人々 びと → ひと).

**Words worth showing.** A candidate must be on a JLPT vocabulary list or
carry a JMdict priority tag on both its form and its reading (news, ichi,
spec, gai, nf), and must not be marked "usually written using kana alone" in
its first sense. Without this filter the most frequent word for a rare reading
can be an archaic one (天 あめ → 天山 あめやま "enormous thing", arch.) or one
a learner never sees in kanji (山 サン → 沢山 たくさん).

**The pick.** Per asked reading, a JLPT list word at or below the kanji's own
JLPT level first (for N5-N1 that is the deck's level; a Jōyō kanji with no
level takes a word from any list), then the highest weight. The kanji alone or
with its okurigana counts as a word (本 ほん, 食べる たべる), as in the mockup's
休む. The N5 picks read like a textbook: 日 毎日 まいにち / 日 ひ, 食 食堂
しょくどう / 食べる, 学 学校 がっこう / 学ぶ, 生 先生 せんせい / 生まれる,
東 東京 とうきょう / 東 ひがし.

**The glosses.** Each picked pair is looked up in the multilingual JMdict,
`JMdict.gz` (every language; created 2026-10-23, 22,205,018 bytes, SHA256
`c60751edd3f4622fd90d964b16add6e92b66e9fad892d1e3dc8eaa2782980862`), not
`JMdict_e`. A pair in several entries takes the entry with the highest
priority. The **English** gloss is the first gloss of the first English sense.
The **Spanish** glosses are every Spanish gloss of the entry, in order: they
sit in senses of their own, with no part of speech, no link to an English
sense and no restriction to a form or reading. 34,310 of JMdict's 218,948
entries (15.7 %) have one (68,288 glosses).

**The shown gloss and "short".** The gloss an intro would show is the first
one that is not a transliteration of the word's reading (romaji with or
without long vowels: "sake"; a quoted one leading an explanation: `"oden": un
tipo de cocido`), after dropping loanword notes (`neurona (eng: neuron)` →
"neurona"). A capitalised transliteration is a proper name and counts as a
gloss ("Chiba", "Tokyo"). **Short** means that shown gloss has at most **24
characters**. At 18 px a lowercase Latin letter of set 2 advances 9.5 px on
average (Noto Sans JP), so a 135 px panel's line (two 4 px gutters) holds
about 13 characters and 24 is about two lines; Spanish glosses need set 2,
where the Spanish packs already keep their accented letters (n5-es has 217
set-2 glyphs, n5-en 212). The same rules are applied to English.

## Results per deck

"Asked on/kun": cards whose kind is not empty. "On/kun word": asked kinds that
got a word. "Spanish/English gloss": words with at least one gloss in that
language; "≤ 24": words whose shown gloss is short. The last column counts
cards with at least one word that has a short Spanish gloss.

| deck | kanji | asked on | on word | asked kun | kun word | words | Spanish gloss | Spanish ≤ 24 | English gloss | English ≤ 24 | cards with a short Spanish word |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| N5 | 79 | 79 | 79 | 72 | 71 | 150 | 149 (99 %) | 144 (96 %) | 150 (100 %) | 145 (97 %) | 78 (99 %) |
| N4 | 166 | 166 | 166 | 134 | 132 | 298 | 291 (98 %) | 287 (96 %) | 298 (100 %) | 277 (93 %) | 165 (99 %) |
| N3 | 367 | 363 | 360 | 285 | 276 | 636 | 617 (97 %) | 603 (95 %) | 636 (100 %) | 604 (95 %) | 365 (99 %) |
| N2 | 367 | 362 | 357 | 243 | 241 | 598 | 563 (94 %) | 545 (91 %) | 598 (100 %) | 554 (93 %) | 359 (98 %) |
| N1 | 1,232 | 1,173 | 968 | 615 | 488 | 1,456 | 1,245 (86 %) | 1,184 (81 %) | 1,456 (100 %) | 1,286 (88 %) | 890 (72 %) |
| Jōyō | 2,136 | 2,059 | 1,994 | 1,317 | 1,248 | 3,242 | 2,965 (91 %) | 2,866 (88 %) | 3,242 (100 %) | 2,971 (92 %) | 1,947 (91 %) |

Asked kinds left without a word by the "worth showing" filter (they have only
rare or usually-kana words): N5 1 (天 あめ), N4 2, N3 12, N2 7, N1 325, Jōyō
133.

**Breakdowns** (every picked word of the six decks, 3,327 words):

| words | count | Spanish gloss | Spanish ≤ 24 | English ≤ 24 |
|---|---:|---:|---:|---:|
| on'yomi words | 2,045 | 88 % | 85 % | 93 % |
| kun'yomi words | 1,282 | 94 % | 91 % | 88 % |
| on a JLPT list at or below the kanji's level | 2,285 | 99 % | 96 % | 93 % |
| on no JLPT list (or a harder one) | 1,042 | 72 % | 67 % | 87 % |
| the kanji's own form (本, 食べる) | 1,280 | 97 % | 93 % | 89 % |
| compounds | 2,047 | 87 % | 83 % | 93 % |
| kanji of JLPT N5 | 150 | 99 % | 96 % | 97 % |
| kanji of JLPT N4 | 298 | 98 % | 96 % | 93 % |
| kanji of JLPT N3 | 636 | 97 % | 95 % | 95 % |
| kanji of JLPT N2 | 598 | 94 % | 91 % | 93 % |
| kanji of JLPT N1 | 1,456 | 86 % | 81 % | 88 % |
| Jōyō kanji with no JLPT level | 189 | 77 % | 74 % | 88 % |

- **Coverage falls with the level, and it is the words that decide it.**
  Words on a JLPT list have a Spanish gloss 99 % of the time (96 % short);
  words on no list 72 % (67 %). N5-N3 kanji find list words for almost every
  reading; N1 kanji often do not, and N1 includes 247 name (jinmeiyō) kanji
  whose readings live in rare words. That is also why N1 has the fewest words
  per asked kind (83 % of on'yomi, 79 % of kun'yomi).
- **Transliterations and length are minor.** Of the 3,011 words with a
  Spanish gloss, the first one is a transliteration for 19 (9 have nothing
  else); the shown gloss is over 24 characters for 100. The first Spanish
  gloss has 8 characters at the median and 16 at the 90th percentile. Over all
  3,327 words, the shown Spanish gloss fits 14 characters (one line on a
  135 px panel) for 79 %, 20 for 85 %, 24 for 87 %, 32 for 89 %.
- **English is not much better on "short".** Every word has an English gloss,
  but the first one of a verb is often long ("to come (spatially or
  temporally)"), so English ≤ 24 is 88-97 % per deck. 333 words have a short
  English gloss and no short Spanish one.

## Spot check

30 random N5-N3 words that have a Spanish gloss (`--seed 9`), with the shown
gloss judged as the meaning to show a learner:

| # | kanji | asked | word | reading | English (first sense) | Spanish glosses | shown | verdict |
|---:|---|---|---|---|---|---|---|---|
| 1 | 与 | on ヨ | 関与 | かんよ | participation; taking part in | intervenir; tomar parte; participar; intervención | intervenir | right (a verb for a noun) |
| 2 | 便 | kun たよ.り | 便り | たより | news; tidings; information | noticias; nuevas; carta | noticias | right |
| 3 | 優 | on ユウ | 優勝 | ゆうしょう | overall victory; championship | quedar campeón; quedar el primero; … | quedar campeón | right |
| 4 | 写 | kun うつ.す | 写す | うつす | to copy; to duplicate | transcribir; duplicar; reproducir; … | transcribir | right |
| 5 | 勉 | on ベン | 勉強 | べんきょう | study | estudio; diligencia; descuento; reducción | estudio | right |
| 6 | 古 | on コ | 中古 | ちゅうこ | used; second-hand; old | de medio uso; de segunda mano | de medio uso | right (regional) |
| 7 | 吹 | kun ふ.く | 吹く | ふく | to blow (of the wind) | soplar | soplar | right |
| 8 | 声 | on セイ | 声優 | せいゆう | voice actor or actress | actor o actriz de voz (radio, animación, doblaje) | (49 characters) | right, too long |
| 9 | 多 | on タ | 多分 | たぶん | probably; likely; perhaps | tal vez; quizás; probablemente; … | tal vez | right |
| 10 | 安 | kun やす.い | 安い | やすい | cheap; inexpensive | barato; calma; pacífico; … | barato | right |
| 11 | 寝 | on シン | 寝室 | しんしつ | bedroom | dormitorio | dormitorio | right |
| 12 | 対 | on タイ | 絶対 | ぜったい | absolutely; definitely | totalmente; incondicionalmente; … | totalmente | right |
| 13 | 店 | kun みせ | 店 | みせ | store; shop | tienda; comercio; establecimiento | tienda | right |
| 14 | 忙 | kun いそが.しい | 忙しい | いそがしい | busy; occupied | ocupado; irritado | ocupado | right |
| 15 | 打 | kun う.つ | 打つ | うつ | to hit; to strike | golpear; pegar; escribir a máquina; … | golpear | right |
| 16 | 政 | on セイ | 政治 | せいじ | politics; government | la política | la política | right |
| 17 | 朝 | kun あさ | 朝 | あさ | morning | la mañana | la mañana | right |
| 18 | 歳 | on サイ | 歳 | さい | ... years old; age (of) ... | habilidad; regalo; talento; aptitud; genio; años de edad | habilidad | **wrong** (才's meaning) |
| 19 | 男 | on ダン | 男性 | だんせい | man; male | sexo masculino; hombre | sexo masculino | doubtful (stilted) |
| 20 | 疑 | on ギ | 疑問 | ぎもん | doubt; question; suspicion | pregunta; duda; adivinanza | pregunta | doubtful ("duda") |
| 21 | 米 | kun こめ | 米 | こめ | (husked grains of) rice | arroz no cocinado | arroz no cocinado | right |
| 22 | 給 | on キュウ | 給料 | きゅうりょう | salary; wages | sueldo; salario | sueldo | right |
| 23 | 置 | kun お.く | 置く | おく | to put; to place | poner; colocar; dejar (atrás); … | poner | right |
| 24 | 試 | kun ため.す | 試す | ためす | to try (out); to test | intentar; examinar | intentar | right |
| 25 | 話 | kun はなし | 話 | はなし | talk; speech; chat | habla; plática; conversación; charla; relato | habla | doubtful ("conversación") |
| 26 | 追 | kun お.う | 追う | おう | to chase; to run after | perseguir; ir detrás de | perseguir | right |
| 27 | 運 | on ウン | 運転手 | うんてんしゅ | driver; chauffeur | chofer; conductor | chofer | right |
| 28 | 過 | kun す.ぎる | 過ぎる | すぎる | to pass through; to pass by | pasar; ir más allá; transcurrir; rebasar | pasar | right |
| 29 | 風 | on フウ | 台風 | たいふう | typhoon; hurricane | tifón | tifón | right |
| 30 | 飲 | kun の.む | 飲む | のむ | to drink; to swallow | tragar; ingerir; tragar entero; …; beber | tragar | doubtful ("beber" is fifth) |

**25 of 30 right** (one of them too long to show), **4 doubtful**, **1 wrong**.
Reading all 150 N5 words gives the same picture, with the errors on the most
basic words: 五 ご "cuándo" (the entry also lists the reading いつ, "when"),
九 "nueve (en documentos legales)" and 四 "cuatro (usado en documentos
legales)" (notes that belong to the legal forms 玖 and 肆), 意見 "aconsejar"
(the verb 意見する, not "opinion"), 以上 "nada menos que"; stilted or secondary
ones: 大丈夫 "estar a salvo", 先 "previo", 出る "aparecer", 女性/男性 "sexo
femenino/masculino". About 8 % of the N5 words would need a fix. Elsewhere:
偲ぶ "recolectar" (a false friend of "recollect"), "tener la intensión de".

The causes are structural, so they recur: the Spanish glosses belong to the
whole entry (every form and reading: 歳/才, 五/いつ), their order is not by
importance (吸う "fumar" before "aspirar"), and a noun often gets a verb
(努力 "esforzarse"). Showing them would need a reviewed override table, as
the meanings have (`data/meaning_overrides.tsv`, 39 lines), for the 1,084
N5-N3 words at least.

## Size

Per deck: today's pack (the `decks-v2` catalog), and what each option would
add. Set 2 holds no kanji today, so every kanji in a word needs an 18 px
set-2 glyph: the card's own kanji and the words' other kanji. Each costs 125 B
on average (a 16 B glyph table entry and the RLE bitmap, rendered from Noto
Sans JP with the builder's own renderer and encoder). Strings are UTF-8 with a
NUL, deduplicated (外国 serves 外 and 国). The index is a guess pending the
spec: a u32 string offset per word, reading and gloss for each asked kind
(8 B per kind for (c), 12 B for (b)), plus a 16 B header extension. (b) stores
a gloss only when it is short.

| deck | today | new set-2 kanji (deck's / other) | glyphs | strings (c) | **(c)** words + readings | strings (b), es | **(b)** + Spanish gloss | **(b)** + English gloss |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| N5 | 104 KB | 79 / 41 | 12.2 KB | 2.3 KB | +16 KB (15 %) | 3.4 KB | +17 KB (17 %) | +17 KB (17 %) |
| N4 | 234 KB | 166 / 79 | 27.6 KB | 4.9 KB | +35 KB (15 %) | 7.3 KB | +38 KB (16 %) | +38 KB (16 %) |
| N3 | 528 KB | 367 / 165 | 63.5 KB | 10.6 KB | +79 KB (15 %) | 15.7 KB | +87 KB (16 %) | +87 KB (16 %) |
| N2 | 521 KB | 366 / 222 | 69.8 KB | 10.2 KB | +85 KB (16 %) | 15.1 KB | +92 KB (18 %) | +92 KB (18 %) |
| N1 | 1,801 KB | 1,045 / 457 | 186.1 KB | 25.2 KB | +225 KB (13 %) | 35.9 KB | +243 KB (13 %) | +245 KB (14 %) |
| Jōyō | 2,961 KB | 2,097 / 12 | 258.4 KB | 50.7 KB | +335 KB (11 %) | 73.2 KB | +371 KB (13 %) | +373 KB (13 %) |

(The English packs are a few hundred bytes smaller today; the additions are the
same to within 2 KB.) The glyphs are about three quarters of the cost and
both options need them; the glosses add only 1-3 points. §3.11's guess of
+10-15 % for a Jōyō pack holds (+11 % / +13 %, 3.3 MB, well under the 5 MB
per-pack cap); the small decks pay more (+15-18 %) because their words bring in
kanji from outside the deck (N2: 222 of them).

## Licence

- **The words and readings** are JMdict's Japanese part: CC BY-SA 4.0 under
  the EDRDG licence, the same terms and attribution as KANJIDIC2 (§3.11).
- **English glosses** (`JMdict_e`, or the English of `JMdict.gz`): covered by
  the same licence.
- **Spanish glosses are not.** The EDRDG licence covers "JMDICT -
  Japanese-Multilingual Dictionary File - the Japanese and English
  components", and says "the translational equivalents in other languages,
  e.g. German, French, Dutch, etc. are covered by separate copyright held by
  the compilers of that material" (https://www.edrdg.org/edrdg/licence.html,
  read 2026-10-23). The file names no compiler or licence for its Spanish
  glosses. docs/meaning-order.md and docs/data-sources.md already rejected
  them for `--meaning-order` on these grounds ("not usable"). So the
  multilingual `JMdict.gz` is **not** simply "EDRDG, CC BY-SA 4.0": only its
  Japanese and English parts are. Shipping its Spanish glosses in the public
  `decks-v2` release would need the EDRDG (or the compilers it names) to
  confirm in writing that they may be redistributed under CC BY-SA 4.0.
- The script reads `JMdict.gz` from the git-ignored cache only to measure;
  nothing from it is committed except the 30 short quotations above.
- Translating the English gloss through FreeDict's English-Spanish dictionary
  (CC BY-SA 3.0, already used by `--meaning-order`) is licensable, but was not
  measured here, and §3.11 expects it to pick wrong senses often.

## Recommendation for decision 9

*Superseded by the revised recommendation at the end of "Alternatives".*

**(c) for the Spanish decks now, in a pack format that already has a gloss
slot, and (b) for the English decks.** Pack v3's words section carries a
gloss offset per word (0 = none, 4 B per asked kind). English packs fill it
from `JMdict_e` (licensed, 88-97 % short per deck); Spanish packs leave it
empty and show the word and its reading.

Reasons:

- **Coverage would support (b) in Spanish** for N5-N2 (91-96 % of the words
  have a short gloss, 98-99 % of the cards get at least one), less for N1
  (81 %, 72 % of the cards).
- **The licence does not.** JMdict's Spanish glosses are outside the EDRDG's
  CC BY-SA grant, and the project already declined them for meanings for that
  reason. That is a question for the EDRDG, not something more measuring
  answers.
- **Their quality would need review anyway**: 5 of 30 sampled N5-N3 glosses
  doubtful or wrong, and about 8 % of N5 (五 "cuándo").
- **The size barely depends on the choice.** (c) costs +11-16 % and (b)
  +13-18 %; the 18 px kanji glyphs are most of either. So the format can carry
  a gloss slot now, and Spanish glosses can arrive later without another
  format change.
- **(c) still teaches.** 38 % of the picked words are the kanji's own form
  (本 ほん, 食べる), whose meaning is the card's own, shown above them; the
  compounds show the asked reading in use (毎日 まいにち), which the intro
  cannot otherwise do.

If the maintainer judges the second intro page (one more press per new card on
the 135 px panels) not worth a word without its meaning, the alternative is (a)
for Spanish: no words until the EDRDG answers, with words in English decks
only.

**Next steps if accepted:** ask the EDRDG about the Spanish glosses' licence;
then phase 4 of docs/learning-mode.md (spec first: the words section in
docs/pack-format.md, v3, set 2's kanji). If the answer allows Spanish glosses,
add them with a reviewed override table for N5-N3 and the short-gloss cut.

## Alternatives

JMdict's Spanish glosses cannot be redistributed (above). This section checks
the licence-compatible sources for the same picked words (3,063 distinct
words, 3,327 word slots, N5-N1 ∪ Jōyō), and where JMdict's Spanish glosses
come from (G).

### How each candidate gives a gloss

- **A. Wordnets** (Open Multilingual Wordnet 1.4 release). The Japanese
  WordNet lists the word's synsets. The one whose English (Princeton WordNet)
  lemmas contain the word's earliest JMdict_e gloss is kept, the most
  frequent English sense first. The gloss is that synset's first Spanish
  lemma, from the Multilingual Central Repository 3.0 (MCR). Of the 3,063
  words, 1,859 resolve. Of the rest, 400 have no synset, 308 have synsets but
  none matches the English, and 496 match a synset with no Spanish lemma.
- **B1. Spanish Wiktionary**: the first Spanish definition of the word's
  Japanese entry (kaikki.org's wiktextract data). The Spanish Wiktionary has
  only 880 Japanese entries written with kanji.
- **B2. English Wiktionary, then its translation tables**: the first English
  gloss of the word's Japanese entry, split at commas ("novel, story,
  fiction"), translated as in C. FreeDict's eng-spa dictionary, which
  `--meaning-order` already pins, *is* English Wiktionary's translation tables
  (WikDict, via DBnary). So this is the "Wiktionary glosses plus translation
  tables" route without the 2 GB English dump.
- **C. JMdict_e through FreeDict**: the first English gloss of the word's
  first sense that FreeDict has (without "to" or an article). The gloss is
  the first Spanish translation of the first sense of FreeDict's entry with
  the same part of speech.
- **D. Wikidata lexemes**: a Japanese lexeme whose lemma is the word (and
  whose hiragana lemma is the reading, when it has one). Only 12 Japanese
  lexemes have a Spanish gloss of their own, so the gloss is the Spanish label
  of the item that the lexeme's first item-linked sense points to (P5137).
- **E. Composition**: no gloss. The intro shows the first Spanish meaning of
  each of the word's kanji, as the published Spanish packs store it:
  休日 = descanso + día.
- **F. A project-authored table**: Spanish glosses drafted from JMdict_e's
  English (with C as the starting suggestion), reviewed by the maintainer,
  and committed like `data/meaning_overrides.tsv`.

### Comparison

Coverage is the share of word slots with a Spanish gloss of at most 24
characters. The spot check is the same 30 random N5-N3 words for every source
(`--seed 9`), each covered word judged right, doubtful or wrong as the meaning
to show a learner (table below). For E it is whether the parts read as the
word's meaning.

| source | licence in CC BY-SA 4.0 packs | short Spanish gloss: N5 / N4 / N3 / N2 (N1, Jōyō) | spot check: right / doubtful / wrong (of covered) | upkeep |
|---|---|---|---|---|
| JMdict Spanish (reference) | **no** (unclear; above) | 96 / 96 / 95 / 91 % (81, 88) | 26 / 3 / 0 (of 29) | — |
| A. Wordnets | **yes**: MCR CC BY 3.0, Japanese WordNet notice | 67 / 73 / 72 / 64 % (55, 62) | 14 / 5 / 1 (of 20) | low: OMW 1.4 frozen since 2021, 49 MB |
| B1. Spanish Wiktionary | **yes**: CC BY-SA 4.0 | 22 / 18 / 6 / 6 % (2, 6) | 6 / 0 / 0 (of 6) | medium: 103 MB, regenerated weekly; useless at this coverage |
| B2. English Wiktionary → FreeDict | **yes**: CC BY-SA 4.0 + FreeDict CC BY-SA 3.0 | 78 / 82 / 86 / 77 % (72, 78) | 14 / 2 / 4 (of 20) | medium: 49 MB, regenerated weekly; C's sense errors too |
| C. JMdict_e → FreeDict | **yes**: both already credited | 96 / 93 / 94 / 88 % (87, 90) | 21 / 1 / 5 (of 27) | low: no new data, ~60 lines |
| D. Wikidata lexemes | **yes**: CC0 | 64 / 50 / 31 / 34 % (23, 31) | 11 / 2 / 1 (of 14) | medium: a query snapshot that changes daily |
| E. Composition | **yes**: KANJIDIC2, already in the packs | 95 / 95 / 98 / 96 % as text (94, 95) | 21 / 4 / 5 (of 30); compounds only: 8 / 3 / 4 (of 15) | low: firmware work + 0.6-7 KB per deck |
| F. Reviewed table, seeded by C | **yes**: project-authored, CC BY-SA 4.0 | what is reviewed, up to 100 % | C's draft right as-is for 22 of 30 N5 words; 6 edits, 1 word to replace | a one-off review: 142 / 410 / 988 / 1,538 words (N5 .. N2, cumulative); a check for new picks |

C agrees with A or D (the same Spanish word) for 53 % of the N5 slots, 46 %
of N4, 43 % of N3 and N2, and 30 % of N1. In the spot check, the 13 words
where C agrees with A or D were 12 right and 1 doubtful, so agreement could
fast-track part of a review. B2 cannot serve that way, because it shares
FreeDict with C.

### Spot check

✓ right, ~ doubtful, ✗ wrong, — no gloss. JMdict's Spanish is shown for
reference only.

| # | word | JMdict_e | JMdict es | A | B1 | B2 | C | D | E |
|---:|---|---|---|---|---|---|---|---|---|
| 1 | 一 いち | one | uno ✓ | — | Uno ✓ | uno ✓ | uno ✓ | Uno ✓ | uno ✓ |
| 2 | 二 に | two | dos ✓ | — | Dos ✓ | dos ✓ | dos ✓ | dos ✓ | dos ✓ |
| 3 | 体 からだ | body | cuerpo ✓ | cuerpo ✓ | — | — | carrocería ✗ | cuerpo ✓ | cuerpo ✓ |
| 4 | 冬 ふゆ | winter | invierno ✓ | invierno ✓ | Invierno ✓ | invierno ✓ | invierno ✓ | invierno ✓ | invierno ✓ |
| 5 | 制限 せいげん | restriction | restricción ✓ | limitación ✓ | — | límite ✓ | restricción ✓ | — | sistema + limitar ~ |
| 6 | 動物 どうぶつ | animal | animal ✓ | animal ✓ | Animal ✓ | animal ✓ | animal ✓ | — | mover + cosa ~ |
| 7 | 午後 ごご | afternoon | la tarde ✓ | tarde ✓ | — | tarde ✓ | tarde ✓ | tarde ✓ | mediodía + después ✓ |
| 8 | 売買 ばいばい | trade | comercio ✓ | trade ✗ | — | — | comercio ✓ | — | vender + comprar ✓ |
| 9 | 小説 しょうせつ | novel | novela ✓ | novela ✓ | Novela ✓ | novela ✓ | novela ✓ | Xiaoshuo ✗ | pequeño + explicar ✗ |
| 10 | 居候 いそうろう | lodger who pays nothing … | — | aprovechador ~ | — | — | gorrón ✓ | — | existir + clima ✗ |
| 11 | 式 しき | way | estilo ✓ | — | — | tipo ~ | camino ✗ | ecuación ~ | forma ✓ |
| 12 | 強盗 ごうとう | robber | ladrón ✓ | atracador ✓ | — | robo ✓ | ladrón ✓ | robo ✓ | fuerte + robo ✓ |
| 13 | 慣れる なれる | to get used to | acostumbrarse ✓ | — | — | — | — | — | acostumbrarse ✓ |
| 14 | 探す さがす | to search for | buscar (…) ✓ long | buscar ✓ | — | buscar ✓ | buscar ✓ | — | buscar ✓ |
| 15 | 教室 きょうしつ | classroom | aula ✓ | clase ~ | — | aula ✓ | aula ✓ | aula ✓ | enseñar + habitación ✓ |
| 16 | 暮れる くれる | to get dark | tornarse oscuro ~ | — | — | caer ✗ | — | — | anochecer ✓ |
| 17 | 死亡 しぼう | death | muerte ✓ | desaparición ~ | — | el arcano de la muerte ✗ | el arcano de la muerte ✗ | muerte ✓ | morir + difunto ✓ |
| 18 | 牛 うし | cattle (Bos taurus) | ganado ✓ | Bos Taurus ~ | — | bovino ~ | ganado bovino ✓ | Bos primigenius taurus ~ | vaca ✓ |
| 19 | 犯罪 はんざい | crime | crimen ✓ | crimen ✓ | — | crimen ✓ | crimen ✓ | crimen ✓ | ofensa + delito ✓ |
| 20 | 王 おう | king | rey ✓ | monarca ✓ | — | rey ✓ | rey ✓ | monarca ✓ | rey ✓ |
| 21 | 社 やしろ | (Shinto) shrine | altar o templo shinto ✓ | santuario ✓ | — | — | santuario ✓ | — | compañía ✗ |
| 22 | 突然 とつぜん | abrupt | abrupto ~ | abrupto ~ | — | repentino ✓ | abrupto ~ | — | súbito + de esa forma ~ |
| 23 | 笑う わらう | to laugh | reír ✓ | reír ✓ | — | — | reír ✓ | — | risa ✓ |
| 24 | 結構 けっこう | splendid | que está bien así como está ~ | — | — | — | espléndido ✓ | — | atar + construir ✗ |
| 25 | 絶える たえる | to die out | extinguirse ✓ | — | — | — | extinguirse ✓ | — | suprimir ~ |
| 26 | 進む すすむ | to advance (spatially) | progresar ✓ | — | — | ascender ✗ | ascender ✗ | — | avanzar ✓ |
| 27 | 銀行 ぎんこう | bank | banco ✓ | banco ✓ | — | orilla ✗ | orilla ✗ | banco ✓ | plata + ir ✗ |
| 28 | 顔面 がんめん | face (of person) | cara (de una persona) ✓ | — | — | — | cara ✓ | — | cara + cara ✓ |
| 29 | 食べる たべる | to eat | comer ✓ | comer ✓ | Comer ✓ | comer ✓ | comer ✓ | — | comer ✓ |
| 30 | 飲食 いんしょく | food and drink | comida y bebida ✓ | — | — | — | — | — | beber + comer ✓ |

What the errors look like:

- **C and B2** pick the dictionary's first sense of an ambiguous English
  word: bank → "orilla" (river bank), death → "el arcano de la muerte" (the
  tarot card), body → "carrocería", to advance → "ascender" (a promotion),
  way → "camino". The same happens outside the sample: money → "pasta"
  (slang), English (language) → "texto", below → "bajo cero".
- **A** is right when it resolves, but often has nothing. Its Spanish lemmas
  can be English or Latin left untranslated ("trade", "Bos Taurus"), or a
  rare synonym ("desaparición" for death).
- **D** is item labels, not word senses: 小説 points to the Chinese genre
  ("Xiaoshuo") and 牛 to the taxon. Coverage collapses after N4.
- **E** reads well for half of the compounds (売買 = vender + comprar, 教室 =
  enseñar + habitación). It misleads for a quarter: 銀行 = plata + ir, 小説 =
  pequeño + explicar, 結構 = atar + construir. It also fails where the word's
  meaning is not the card's (社 やしろ "shrine" shows the card's
  "compañía"). E needs no pack strings for words whose kanji are all in the
  deck (71 % of N5's word slots, 64 % of N4, 68 % of N3, 51 % of N2, 49 % of
  N1, 100 % of Jōyō). The other kanji need a meaning string each: 40 kanji
  (0.6 KB) for N5, 79 (1.2 KB) for N4, 164 (2.5 KB) for N3, 222 (3.4 KB) for
  N2, 456 (6.9 KB) for N1, 11 for Jōyō. That is a small extra table of code
  point and offset, plus firmware to look the meanings up.

### Licences

- **A.** MCR 3.0 (`omw-es/LICENSE` in the release): "All other data in this
  package are distributed under Attribution 3.0 Unported (CC BY 3.0)
  license" (the English WordNet part is under the WordNet licence). The
  Japanese WordNet (`omw-ja/LICENSE`): "Permission to use, copy, modify and
  distribute this software and database and its documentation for any
  purpose and without fee or royalty is hereby granted, provided that you
  agree to comply with the following copyright notice and statements,
  including the disclaimer, and that the same appear on ALL copies …"
  (Copyright 2009 NICT). Princeton WordNet has the same terms. Packs would
  carry only MCR's Spanish lemmas: CC BY 3.0 material may go into a CC BY-SA
  4.0 pack with attribution, and the NICT and Princeton notices would join the
  credits because their data chose the synset. Release:
  https://github.com/omwn/omw-data/releases/tag/v1.4; Japanese WordNet
  https://bond-lab.github.io/wnja/; MCR https://adimen.si.ehu.es/web/MCR.
- **B.** Spanish Wiktionary: "El texto está disponible bajo la Licencia
  Creative Commons Atribución-CompartirIgual 4.0" (page footer,
  https://es.wiktionary.org). English Wiktionary: "Permission is granted to
  copy, distribute and/or modify the text of all Wiktionary entries under the
  terms of the Creative Commons Attribution-ShareAlike 4.0 International
  License" (https://en.wiktionary.org/wiki/Wiktionary:Copyrights; also
  GFDL, not needed). Packs would credit "Wiktionary contributors" with a link
  to the entries, which the page says satisfies attribution. kaikki.org adds
  no licence of its own and asks for a link and a citation (Ylonen 2022,
  LREC) as a courtesy.
- **C.** Nothing new. JMdict_e is CC BY-SA 4.0 and FreeDict eng-spa CC BY-SA
  3.0, both already in the packs' credits (docs/data-sources.md).
- **D.** "All structured data (i.e. the main, Property, Lexeme, and
  EntitySchema namespaces) is released into the public domain", under CC0
  (https://www.wikidata.org/wiki/Wikidata:Licensing). Item labels are main
  namespace, so CC0 too. No attribution is required.
- **E.** KANJIDIC2, already in the packs.
- **F.** Written by the project from JMdict_e's English (CC BY-SA 4.0) and,
  where C's suggestion is kept, FreeDict (CC BY-SA 3.0): distributed under CC
  BY-SA 4.0 with both credits, like the meaning overrides.

### G. Who compiled JMdict's Spanish glosses

- **How they get into JMdict.** The EDRDG's project page says the full JMdict
  includes "Spanish (39,000)" entries. "The Japanese headwords are matched
  against the dictionary files for the other languages, and glosses are
  included where there is a match", with "no consistency between the
  dictionaries as to the sense splitting"
  (https://www.edrdg.org/wiki/JMdict-EDICT_Dictionary_Project.html). That
  matching by headword explains 五 "cuándo".
- **The Spanish file.** WWWJDIC's documentation
  (https://www.edrdg.org/wwwjdic/wwwjdicinf.html) names it JSPANISH, "a
  combination of two dictionaries":
  - "the 26,000 entry "HISPADIC" Japanese-Spanish dictionary from that
    wonderful collaborative project" (http://hispadic.byethost3.com/, which
    no longer resolves; Jim Breen added it on 2009-10-29, edict-jmdict list);
  - "the 20,000 entry "RUI" Japanese-Spanish dictionary compiled by Francisco
    Barberan and included with his permission".
- **Licences found.**
  - **HISPADIC**: a Spanish translation of EDICT. Its blog
    (http://hispadic.blogspot.com, last post 2011-10-07) speaks of "la
    licencia creative commons de Hispadic" without naming the licence or its
    version. A derived dictionary says CC BY-SA 3.0 (secondary source,
    unverified).
  - **RUI**: no public licence, only the permission given to the EDRDG. Its
    old page (nichiza.com) is now a consultancy's site.
  - **The EDRDG licence** leaves both to "the compilers of that material".
- **Verdict: unclear.** The RUI half has no licence anyone else can rely on.
  The way to settle it would be a question to the EDRDG (its site and the
  edict-jmdict mailing list); HISPADIC has no contact left. Nothing was sent.

### F. What a review would look like

Words to review per deck, counting each word once (the same word can serve
two kanji): N5 142, N4 268 more (410 in all), N3 578 more (988), N2 550 more
(1,538), N1 1,352 more (2,890), and the Jōyō deck's other kanji 173 more
(3,063). With C's suggestion as the draft, most rows are a confirmation. At
15-30 s a word that is roughly 4-8 hours for N5-N3, and 2-5 more for N2.
Picks change when the word data changes (JMdict_e is fetched daily), so the
builder would need a check, like `check_overrides`, that lists the picked
words without a reviewed gloss and the rows no longer picked.

**DRAFT, not reviewed**: 30 random N5 words (`--seed 9`) with C's suggestion
and a gloss written for this document from the JMdict_e English. The last
column is for the maintainer.

| # | word | JMdict_e (first sense) | C suggestion | draft gloss (DRAFT) | review |
|---:|---|---|---|---|---|
| 1 | お金 おかね | money | pasta ~ (slang) | dinero | |
| 2 | 万 まん | 10,000; ten thousand | diez mil ✓ | diez mil | |
| 3 | 三つ みっつ | three | tres ✓ | tres (cosas) | |
| 4 | 下 した | below; down; under | bajo cero ✗ | debajo; abajo | |
| 5 | 九つ ここのつ | nine | nueve ✓ | nueve (cosas) | |
| 6 | 二つ ふたつ | two | dos ✓ | dos (cosas) | |
| 7 | 今 いま | now; the present time | ahora ✓ | ahora | |
| 8 | 今月 こんげつ | this month | — | este mes | |
| 9 | 入る はいる | to enter; to come in | entrar ✓ | entrar | |
| 10 | 円 えん | yen (currency of Japan) | yen ✓ | yen | |
| 11 | 十 とお | ten; 10 | diez ✓ | diez (cosas) | |
| 12 | 午後 ごご | afternoon; p.m. | tarde ✓ | tarde; p. m. | |
| 13 | 友達 ともだち | friend; companion | amigo ✓ | amigo, amiga | |
| 14 | 土曜日 どようび | Saturday | sábado ✓ | sábado | |
| 15 | 外国人 がいこくじん | foreigner | extranjero ✓ | extranjero | |
| 16 | 学ぶ まなぶ | to learn; to study | aprender ✓ | aprender | |
| 17 | 後 あと | behind; rear | detrás ~ | después; detrás | |
| 18 | 意見 いけん | opinion; view | opinión ✓ | opinión | |
| 19 | 日 ひ | day; days | día ✓ | día | |
| 20 | 最高 さいこう | best; supreme; wonderful | mejor ~ | lo mejor; genial | |
| 21 | 木曜日 もくようび | Thursday | jueves ✓ | jueves | |
| 22 | 本木 もとき | original stock | — | (rare word: pick another) | |
| 23 | 来年 らいねん | next year | el año próximo ✓ | el año que viene | |
| 24 | 生まれる うまれる | to be born | nacido ~ | nacer | |
| 25 | 白い しろい | white | blanco ✓ | blanco | |
| 26 | 祖母 そぼ | grandmother | abuela ✓ | abuela | |
| 27 | 英語 えいご | English (language) | texto ✗ | inglés (idioma) | |
| 28 | 読む よむ | to read | leer ✓ | leer | |
| 29 | 金曜日 きんようび | Friday | viernes ✓ | viernes | |
| 30 | 高い たかい | high; tall | alto ✓ | alto; caro | |

### Revised recommendation for decision 9

**(b) in both languages: English glosses from JMdict_e, Spanish glosses from
a reviewed, project-authored table (F) seeded by C, rolled out level by
level.** Until a level is reviewed, its Spanish words show word and reading
only, which is (c). The pack format is unchanged from the first
recommendation (a gloss slot per word, 0 = none).

Reasons:

- **No automatic source is good enough to ship unreviewed.**
  - **C** has the coverage (93-96 % for N5-N3), but 6 of its 27 sampled
    glosses are wrong or doubtful (bank → "orilla"), so a learner would meet
    an error about once every five words. That is worse than JMdict's own
    Spanish, which would have needed a review too.
  - **A and D** are more often right when they answer, but answer for only
    31-73 % of the words.
  - **B1** barely exists, and **B2** inherits C's sense errors.
  - **E** misleads on a quarter of the compounds.
- **F's licence is clean.** It is derived from licensed English (JMdict_e,
  CC BY-SA 4.0) with FreeDict suggestions (CC BY-SA 3.0, already credited).
  Its quality is the reviewer's, and it can be fixed line by line, like the
  meaning overrides.
- **The review is bounded and mostly confirmation.** 142 N5 words, 988 for
  N5-N3 and 1,538 for N5-N2. C's draft is right as-is for about three words
  in four, and agreement with A or D can flag the safe half.
- **The other options are fallbacks.** If the maintainer does not want to
  review, the first recommendation stands: (c) for Spanish. Waiting for an
  answer about JMdict's Spanish glosses (G) is unlikely to pay off, because
  half of them (RUI) were given to the EDRDG by permission only.

## Reproducing

```sh
cd tools/pack-builder
uv run tamagotcho-pack fetch --readings                     # JMdict_e, JmdictFurigana, wordfreq, Tatoeba, Jōyō table, JLPT lists
uv run python scripts/example_words_report.py --fetch       # also JMdict.gz (all languages, 22 MB) -> cache/readings/
gh release download decks-v2 -D out/decks-v2                # today's packs, for the size estimate
uv run python scripts/example_words_report.py --packs out/decks-v2 --tsv out/example-words.tsv
```

`--max-gloss N` changes the "short" cut, `--seed`/`--sample` the spot check.
`JMdict.gz` is rebuilt daily at an unversioned URL, so a later run can differ
slightly; the report prints the file's date and SHA256. Keep the `--tsv`
output (every picked word with its glosses) out of the repository: it holds
the Spanish glosses. A run takes about two minutes.

The alternatives (also needs `fetch --meanings` for FreeDict):

```sh
uv run python scripts/example_words_alternatives.py --fetch --packs out/decks-v2 --tsv out/alternatives.tsv
```

`--fetch` downloads, into `cache/research/` (~200 MB, git-ignored), the OMW
1.4 release, kaikki.org's English Wiktionary Japanese extract and Spanish
Wiktionary raw extract, and a Wikidata query result. Each is checked against
the SHA256 pinned in the script as fetched on 2026-10-23. The kaikki.org files
(extracted 2026-10-21 from the enwiktionary dump of 2026-10-01) and the
Wikidata result are regenerated upstream, so a later `--fetch` stops at the
check until the pins are reviewed and updated. A run takes about two minutes
after the picks.
