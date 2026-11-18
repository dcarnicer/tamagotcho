# Meanings in a useful order

Status (2026-10-20): researched and implemented on the branch
`feature/es-meanings` as `tamagotcho-pack build --meaning-order`. The first
version ordered the Spanish meanings by KANJIDIC2's English order (the
maintainer's decision); after reviewing it, the maintainer decided (second
version, below) to order the **English** meanings first by a learner signal,
to order the Spanish ones by that improved English order, to use it for the
English decks too, and to add a short hand-checked override table.
`decks.yml` is prepared to use the flag for the published decks but has not
been run: the maintainer reviews the N5-N3 changes first. Without the flag the
builder's output is byte-identical to before.

## Problem

The device shows a card's **first** meaning: as the quiz option, in the "which
kanji means ..." prompt and first on the detail card.

- KANJIDIC2's Spanish list is often in another order than its English one:
  持 es "duración; propiedad; cargo; sostener; mantener" against en "hold;
  have", 界 "borde; frontera; extensión; ámbito" against "world; boundary".
- Following KANJIDIC2's English order reproduces the English list's own
  quirks: 機 "loom; mechanism; machine" (telar), 原 "meadow; original"
  (prado), 私 "private; I; me" (privado), 明 "bright; light" (inteligente),
  験 "verification; effect; testing" (efecto), 背 "stature; height; back"
  (estatura), 支 "branch; support" (rama), 頂 "place on the head; receive"
  (colocar sobre), 更 "grow late; night watch; …" (caer la noche). English
  decks show those first meanings today (機 "loom").

The invariant must keep holding: **no option may be another meaning of the
target** (docs/roadmap.md).

## How meanings flow

**Builder** (`tools/pack-builder`):

- `kanjidic.parse_kanjidic` takes every `<meaning>` in KANJIDIC2's order,
  grouped by `m_lang` (none = English).
- `kanjidic.make_cards` takes the requested language's list; when the kanji
  has none it takes English and sets card flag bit 0 (English fallback; 73 of
  the 2,383 default-deck kanji have no Spanish meaning). A non-English list is
  cut to **as many meanings as English has**, then every list to **three**
  (`MAX_MEANINGS`). Spanish lists have 3.5 meanings on average and English
  ones 3.6, so the cut matters: 持 kept "duración; propiedad", the two glosses
  that translate neither "hold" nor "have"; 機 keeps "loom; mechanism;
  machine".
- `pack.card_strings` joins them with `; `.
- `distractors.meaning_distractors` drops every candidate card sharing a stored
  meaning with the target (case-insensitive), then ranks by JLPT level, shared
  radical, visual similarity and stroke count, 12 per card.
- WaniKani packs have English meanings only (`fr`/`pt`/`es` fall back with
  flag bit 0).

**Pack** (docs/pack-format.md, card `meanings`): `; `-separated, "most common
first". No other rule.

**Firmware** (`quiz/Quiz.cpp`, `main.cpp`):

- Only the **first** meaning is ever an option or a prompt
  (`kOptionMeanings = 1`): the correct option of a meaning question, each
  distractor's text, and the prompt of a meaning -> kanji question.
- `tryAdd` rejects a distractor whose first meaning is **any stored** meaning
  of the target, and, in meaning -> kanji mode, a candidate that has the
  prompt among its stored meanings.
- The mistake detail card shows the stored meanings (at most three lines).
- Flag bit 0 is not used by the UI.

So reordering inside a list changes what the device shows with no format or
firmware change. Because of the caps it also changes **which** meanings a card
stores: the reordered list keeps the useful ones.

## Sources

Packs are redistributed under CC BY-SA 4.0 in the public `decks-v2` release.
A pack built with the flag contains only KANJIDIC2's own meanings in another
order, but the order is derived from these sources, so they must allow that
use and are credited. No machine translation service or language model is
used: the build is reproducible and offline once the cache is filled.

| source | what it gives | licence | verdict |
|---|---|---|---|
| **JLPT vocabulary lists** (Jonathan Waller, tanos.co.uk; already fetched by `fetch --readings`) | 7,224 N5-N1 (word, reading) pairs | CC BY (docs/common-readings.md) | **used** (learner signal): which words a learner meets with the kanji, and how early |
| **JMdict** (`JMdict_e`, English; already fetched by `fetch --readings`) | the English glosses of those words, by sense | CC BY-SA 4.0 (EDRDG), its Japanese and English parts | **used** (learner signal): 7,184 of the words found |
| wordfreq (already in the cache) | word frequencies | CC BY-SA 4.0 | tried as an extra weight, no gain on the test cases: **not used** |
| **FreeDict eng-spa** (WikDict) | 64,258 English headwords with their Spanish translations, per sense, from Wiktionary via DBnary | **CC BY-SA 3.0** (the TEI header and `COPYING`); a later version (4.0) is allowed for adaptations (§4(b)) | **used** (Spanish by English): a dated release (`2025.11.23`), 3.7 MB, SHA256-pinned |
| **the override table** (`tools/pack-builder/data/meaning_overrides.tsv`) | 39 hand-checked first meanings | written by the project, distributed with the packs under CC BY-SA | **used** (last step) |
| WikDict SQLite (`en-es`, `es-en`, `en-fr`, `en-pt`) | the same data, newer, both directions and French/Portuguese | CC BY-SA | not used: one unversioned URL regenerated in place, cannot be pinned. Only used to estimate fr/pt (below) |
| kaikki.org (wiktextract) | English Wiktionary | Wiktionary's (CC BY-SA, GFDL) | not used: 1.05 GB for the Spanish file, 24 GB raw, regenerated weekly at fixed URLs |
| **full multilingual JMdict** (`JMdict.gz`) | 68,452 Spanish glosses next to the English ones | **unclear**: the EDRDG licence covers "the Japanese and English components (the translational equivalents in other languages, e.g. German, French, Dutch, etc. are covered by separate copyright held by the compilers of that material)" | **not usable** (prototyped: 281 more Spanish glosses matched) |
| Apertium `apertium-en-es` | English-Spanish bilingual dictionary | GPL-2.0 | not usable (not compatible with CC BY-SA 4.0 output) |

URLs: FreeDict database (release, checksum) https://freedict.org/freedict-database.json,
download https://download.freedict.org/dictionaries/eng-spa/2025.11.23/ ·
WikDict https://www.wikdict.com/page/about, files https://download.wikdict.com/dictionaries/sqlite/2/ ·
DBnary licence https://kaiko.getalp.org/about-dbnary/ ("Dbnary is derived from
Wiktionary and is distributed under Creative Commons Attribution-ShareAlike
3.0") · CC BY-SA 3.0 §4(b) https://creativecommons.org/licenses/by-sa/3.0/legalcode ·
kaikki.org https://kaikki.org/dictionary/rawdata.html · EDRDG licence
https://www.edrdg.org/edrdg/licence.html · Apertium https://github.com/apertium/apertium-en-es ·
tanos.co.uk "Use my data!" https://web.archive.org/web/20250117052055/https://www.tanos.co.uk/jlpt/sharing/.

**FreeDict's licence chain.** Wiktionary (CC BY-SA, GFDL) → DBnary (Gilles
Sérasset, extracts the translations; dataset CC BY-SA 3.0) → WikDict (Karl
Bartel, builds the bilingual dictionaries, CC BY-SA) → FreeDict release
`eng-spa 2025.11.23` ("English-español FreeDict+WikDict dictionary"; TEI
`availability`: "Licensed under the Creative Commons Attribution-ShareAlike
3.0 Unported license"; the archive ships the licence as `COPYING`). CC BY-SA
3.0 §4(b) lets an adaptation be distributed under "a later version of this
License with the same License Elements", so it fits the packs' CC BY-SA 4.0, as
KanjiVG's CC BY-SA 3.0 already does.

## Method (`tamagotcho_pack/meanings.py`)

### A. English, by a learner signal (`EnglishOrder`)

For a kanji, every JLPT vocabulary word written with it (飛行機, 機械, 機会
for 機) is looked up in JMdict by word and reading (by word alone when JMdict
pairs it with another reading). Each KANJIDIC2 English meaning scores, summed
over those words:

- the **word's weight**: 16 / 8 / 4 / 2 / 1.5 for N5 .. N1 (the readings'
  learner weights), divided by the number of kanji in the word (the word 私 is
  all 私; 機械 is half 機);
- times its **best match** among the word's first three senses: sense *s*
  weighs 1/(1+*s*), the *p*-th gloss of a sense 1/(1 + 0.2 *p*), and a match is
  1 when the meaning and the gloss have the same stemmed content words ("to
  come (spatially …)" = "come", "going" = "go"), 0.25 when the meaning is part
  of a longer gloss ("next" in "next week"; less for longer glosses), 0.5 for
  two one-word glosses of one family ("original" / "origin").

A score below **2** (an N3 compound's first gloss, or an N4 compound's second)
counts as 0, so a meaning needs more than one rare word to move. The English
list is sorted by score, ties (including all zeros) in KANJIDIC2's order.
私: the N5 word 私 ("I; me") and its compounds give "I" 17.5, "me" 14.6,
"private" 8.4 (from compounds); 明: 明るい ("light; bright") gives "light" 23.8 and
"bright" 17.2; 機: "airplane" 6.2 (飛行機, N5), "opportunity" 4.1 (機会, N4),
"mechanism" 2.4 and "machine" 2.3 (機械, N3), so 機 needs an override.

### B. Spanish, by the English order (`MeaningOrder`)

As in the first version, each Spanish gloss is matched to the English gloss
it translates, now against the reordered English list:

1. **Normalisation**, both languages: lower case, parentheses removed
   (KANJIDIC2 splits a few glosses inside them: "llevar (en coche", "etc.)"),
   accents ignored, no leading "to"/"to be"/"be" or Spanish article; a Spanish
   gloss is also tried without a reflexive `-se` (levantarse) and without a
   trailing preposition (debajo de), an English one by its lemma when the
   dictionary lacks the form (going -> go).
2. **Translation**: the Spanish gloss is the English one (hospital, China) or
   one of its FreeDict translations.
3. **Related form**, only when the gloss translates none of the English glosses
   up to that point: a word sharing a long stem with a translation (préstamo /
   prestar for "lend"), a cognate (matrimonio / matrimony, Japón / Japan: five
   letters or more, the first three shared, at most 40 % edited), the head words
   of a multi-word gloss (pájaro grande / large bird), or a translation of a
   near-synonym, an English word sharing translations with it (debajo
   translates "under", which shares bajo and abajo with "below").
4. **Order**: each Spanish gloss scores the index of the first English gloss it
   matches; a related form counts **one position later**. Sort by score;
   **ties keep KANJIDIC2's order**; **unmatched glosses go last**.

### C. Overrides

`tools/pack-builder/data/meaning_overrides.tsv`, 71 lines, written by the
project for clear errors left in N5-N1 (es, fr and pt): kanji, language, the
meaning to put first, optionally more to follow it, and the reason. Every
listed meaning must be one of KANJIDIC2's own meanings for that kanji and
language (no new text): the builder checks it and refuses to build otherwise,
and a test checks the committed file against KANJIDIC2. An English override
comes before step B, so the Spanish order follows it (機 en "machine" gives es
"máquina").

The maintainer's review of the doubtful N5-N4 cases added 12 lines: 学
estudiar (the verb; KANJIDIC2 has no "aprender"), 円 yen, 着 wear / vestir
(N4: the clothing sense comes first), 去 past / pasado (去年), 料 fee / precio
(料金), 好 gustar, 曲 melody / melodía (a tune; "music" is 楽's). 台 keeps
"pedestal" (an N4 kanji here) and 主 "jefe".

The N2 review (2026-11-18) added 9 lines: 額 amount / cantidad de dinero (金額,
and the forehead; "framed picture" is the compound 額縁), 灯 lamp / lámpara,
泊 alojarse (the intransitive 泊まる; "hospedar" is transitive), 版 edición
(出版, 初版; "impresión" is the printing), 営 negocio (営業, 経営; the camp sense
is rare), 貨 dinero (貨幣, 貨物), 訓 lección (the long 訓読み sense goes second).

The N1 review (2026-11-18) added 15 more: 霞 neblina (the noun; 霞む is the
verb), 施 distribuir (施し "almsgiving" is one sense), 趣 significado
("proceder" is a related form), 慎 prudencia (慎む is the verb), 坪 unidad de
superficie (the long two-tatami gloss overflows), 豆 legumbres, 概
aproximadamente (概ね), 脚 pie ("pata" is an animal leg), 甚 tremendamente
(甚だ), 隊 grupo ("partido" reads as a political party), 椎 roble (the mallet
is 槌), 洞 cavidad, 篤 sincero, 票 voto (not "etiqueta") and 繁 exuberante
(繁雑 is the "complex" sense).

Then the builder's existing caps apply (as many as English has, at most
three). Nothing is added, removed or rewritten.

**Languages.** `en`: steps A and C. `es`, `fr` and `pt`: A, B and C (English
is reordered too: fallback cards show it). German and Italian are not offered:
KANJIDIC2 has no meanings in those languages, so there is no base list to
reorder (the pinned dictionaries only translate English).

**The invariant.** With the flag, `Card.valid_meanings` holds every KANJIDIC2
meaning of the card in its language (before the caps), and
`meaning_distractors` also drops a candidate whose first meaning is one of the
target's meanings, or whose meanings include the target's first, stored or
not, in every language. KANJIDIC2 has meanings beyond the cap in English too,
and default packs break the invariant through them (N3 en: 取's "take" is
offered for 受, whose fourth meaning is "take"; 9 findings in N3 es, 14 in N3
en). The firmware checks the stored meanings only, so the unstored ones matter
only when the precomputed list (12 cards) runs out and the device fills the
options from a random walk.

### Choices tested

**Learner signal** (prototype; "targets" = the 29 cases the maintainer listed:
11 to fix, 18 good changes of the first version to keep, scored on the
Spanish first meaning before overrides):

| variant | English first meanings changed | targets met | notes |
|---|---:|---:|---|
| words weighted 1/(number of kanji), partial matches 0.5 | 801 | 25 | 来 "next" (来年, 来月, 来週) beats "come" |
| 1/(number of kanji)² | 810 | 22 | compounds count too little: 原 field, 種 seed |
| the kanji alone (私, 持つ) counted twice | 811 | 22-23 | 種 seed, 眠, 原 |
| × word frequency (wordfreq) | 799 | 25 | no gain |
| scores within ×1.5 as ties | 721 | 24 | 明 keeps "bright" (inteligente) |
| partial matches 0.25 | 801 | 25 | 来 "come" |
| **partial 0.25 + minimum score 2 (chosen)** | **534** | **25** | drops moves resting on N1-N2 compounds alone (院 temple, 心 mind, 真 reality) |
| partial 0.25 + minimum score 3 | 409 | 24 | 験 keeps "verification" (efecto) |

The four targets the signal cannot reach went to the override table: 申 (the
English list has no "say"), 計 (Spanish has no gloss for "plan" but "pensar"),
機 (飛行機 outweighs 機械) and 中 (the signal picks "middle": 真ん中, 途中, 中心,
中央; the Spanish "dentro" is kept as asked).

**Spanish matching** (first version, still valid):

| matching | glosses matched | kanji with nothing matched |
|---|---:|---:|
| normalisation only (same word) | 125 (2 %) | 2,193 |
| + cognates | 1,202 (15 %) | 1,433 |
| FreeDict translations (with normalisation) | 4,276 (53 %) | 303 |
| + related forms (chosen) | 5,457 (67 %) | 89 |

- **Tie-breaks.** The dictionary's sense order and "translates more of the
  English glosses first" were tried within one English gloss (136 and 215
  first meanings differ). Each fixes some cards (小 pequeño, 天 cielo) and
  breaks others (青 verde before azul, 時 hora, 名 reputación, 夜 tarde). Ties
  keep KANJIDIC2's order. (明, the case that motivated them, is fixed by the
  learner signal: "light" comes before "bright".)
- **Related forms one position later.** Without the penalty (122 first
  meanings differ) a related form ties with the exact translation and often
  comes first (貸 préstamo before prestar, 教 enseñanza before enseñar).
- **Cognates even next to the dictionary's own translations**: restricting them
  changed 8 first meanings, 7 for the worse (姻 matrimonio, 改 reforma).

### French and Portuguese

`decks.yml` accepts `fr` and `pt`. KANJIDIC2 has 7,643 French and 6,963
Portuguese meanings; both are reordered by the same method with **WikDict's
SQLite exports** en-fr and en-pt (release `2_2026-06`, CC BY-SA 4.0, pinned
and checked against SHA256). They are smaller than the Spanish TEI's 64k
headwords — 168,195 and 67,413 `simple_translation` rows — so fewer glosses
match: in the N5-N4-N3 builds the first meaning changes for 9, 47 and 140
French cards and 11, 46 and 154 Portuguese ones (Spanish: 14, 45 and 129).
FreeDict's own eng-fra/eng-por were not used: they are GPL-2+, which cannot be
mixed into the CC BY-SA packs. Eight fr/pt overrides cover the same clear
errors as the Spanish ones: 安 pas cher, 計 mesure / medida, 帰 retour à la
maison / Retôrno a casa, 私 je / Eu and 失 perda. German and Italian are not
supported: KANJIDIC2 has no meanings in those languages, and WikDict only
translates English, so there is no base list to reorder.

## Results

`uv run python scripts/meaning_order_report.py` (after `fetch --readings
--meanings`) prints all of this, and `--tsv` writes every change. 2,383 kanji
(JLPT N5-N1 ∪ Jōyō).

- **English**: first meaning changed for 524 kanji, stored meanings for 258;
  19 English overrides.
- **Spanish** (2,310 kanji with Spanish meanings): 5,457 of 8,124 glosses
  matched to an English gloss; first meaning changed for 707 (against
  KANJIDIC2), stored meanings for 615; 20 Spanish overrides. Against the first
  version, the first Spanish meaning differs for 298.

| | N5 | N4 | N3 | N2 | N1 | no level |
|---|---:|---:|---:|---:|---:|---:|
| kanji | 79 | 166 | 367 | 367 | 1,232 | 172 |
| English first meaning changed | 14 | 49 | 164 | 125 | 157 | 15 |
| English stored meanings changed | 7 | 30 | 98 | 52 | 66 | 5 |
| Spanish first meaning changed | 14 | 45 | 129 | 115 | 370 | 34 |
| Spanish stored meanings changed | 15 | 40 | 135 | 90 | 311 | 24 |
| Spanish first differs from the first version | 6 | 30 | 100 | 69 | 89 | 4 |

**Sanity table.** First meaning, KANJIDIC2 → `--meaning-order`; *v1*: the first
version's Spanish; the stored lists.

| kanji | English | Spanish | v1 | stored English | stored Spanish |
|---|---|---|---|---|---|
| 持 | hold | duración → **sostener** | sostener | hold; have | sostener; mantener |
| 界 | world | borde → **frontera** | frontera | world; boundary | frontera; borde |
| 題 | topic → **subject** | objeto → **tema** | tema | subject; topic | tema; materia |
| 院 | Inst. | institución | institución | Inst.; institution; temple | institución; templo; hospital |
| 安 | relax → **cheap** | barato | barato | cheap; peaceful; quiet | barato; tranquilo; pacífico |
| 重 | heavy | pesado | pesado | heavy; important; respect | pesado; importante; apilar |
| 生 | life | vida | vida | life; birth; genuine | vida; nacimiento; dar a luz |
| 行 | going | ir | ir | going; carry out; conduct | ir; realizar; llevar a cabo |
| 明 | bright → **light** | luz | inteligente | light; bright | luz; claro |
| 気 | spirit | espíritu | espíritu | spirit; mood; mind | espíritu; mente |
| 日 | day | día | día | day; sun; Japan | día; sol; Japón |
| 上 | above | arriba | arriba | above; up | arriba; encima |
| 下 | below → **down** | debajo | debajo | down; below; descend | debajo; debajo de; inferior |
| 大 | large → **big** | grande | grande | big; large | grande; muy |
| 人 | person | persona | persona | person | persona |
| 中 | in → **middle** | centro → **dentro** (override) | dentro | middle; inside; center | dentro; centro; medio |
| 女 | woman | femenino → **mujer** | mujer | woman; female | mujer; femenino |
| 子 | child | chiquillo → **niño** | niño | child; sign of the rat; 11PM-1AM | niño; chiquillo |
| 書 | write | escritura → **escribir** | escribir | write | escribir |
| 毎 | every | siempre → **cada** | cada | every | cada |
| 聞 | hear | preguntar → **escuchar** | escuchar | hear; listen; ask | escuchar; oír; preguntar |
| 見 | see | vista → **ver** | ver | see; look at; visible | ver; poder verse; opinión |
| 食 | eat | alimento → **comer** | comer | eat; food | comer; alimento |
| 小 | little → **small** | poco → **pequeño** | poco | small; little | pequeño; poco |
| 天 | heavens | paraíso | paraíso | heavens; sky; imperial | paraíso; cielo; imperial |
| 先 | before | antes | antes | before; ahead; previous | antes; adelante; previo |
| 半 | half | mitad | mitad | half; semi-; odd number | mitad; medio; semi- |
| 後 | behind | después | después | behind; back; later | después; atrás; detrás |
| 前 | in front → **before** | antes | antes | before; in front | antes; delante de |
| 私 | private → **I** | yo | privado | I; me; private | yo; privado; secreto |
| 機 | loom → **machine** (override) | mecanismo → **máquina** | telar | machine; airplane; opportunity | máquina; avión; ocasión |
| 原 | meadow → **original** | origen | prado | original; plain; field | origen; campo; pradera |
| 験 | verification → **testing** | prueba | efecto | testing; verification; effect | prueba; efecto; intento |
| 計 | plot → **plan** | medida → **medir** (override) | pensar | plan; measure; plot | medir; pensar; medida |
| 申 | have the honor to | decir (override) | tener el honor de | have the honor to; sign of the monkey; 3-5PM | decir; tener el honor de |
| 背 | stature → **back** | espalda | estatura | back; height; stature | espalda; detrás; estatura |
| 支 | branch → **support** | soporte | rama | support; sustain; branch | soporte; apoyo; sostener |
| 頂 | place on the head → **receive** | cima → **recibir** | colocar sobre | receive; summit; top | recibir; cima; cumbre |
| 更 | grow late → **again** | renovar → **de nuevo** | caer la noche | again; more and more; further | de nuevo; caer la noche; renovar |
| 御 | honorable | prefijo honorífico (override) | manipular | honorable; manipulate; govern | prefijo honorífico; manipular; gobernar |
| 処 | dispose → **deal with** | lugar → **tratar** | deshacerse de | deal with; dispose; manage | tratar; deshacerse de; arreglárselas |
| 積 | volume → **pile up** | amontonar | producto (matemáticas) | pile up; volume; product (x*y) | amontonar; apilar; producto (matemáticas) |
| 言 | say | palabras → **decir** | decir | say; word | decir; palabras |
| 帰 | homecoming | regreso → **volver** | volver | homecoming; arrive at; lead to | volver; regreso; vuelta |
| 教 | teach | educación → **enseñar** | enseñar | teach; faith; doctrine | enseñar; enseñanza; educación |
| 失 | lose | pérdida → **perder** | perder | lose; error; fault | perder; error; defecto |
| 難 | difficult | desastre → **difícil** | difícil | difficult; trouble; impossible | difícil; dificultad; imposible |
| 種 | species → **kind** | semilla → **tipo** | tipo | kind; variety; seed | tipo; clase; variedad |
| 眠 | sleep | somnoliento → **dormir** | dormir | sleep; sleepy; die | dormir; somnoliento; tener sueño |
| 来 | come | venir | venir | come; next; become | venir; llegar; acercarse |

**JLPT N5, every first-meaning change** (KANJIDIC2 → new):

- English (14): 円 circle → round, 下 below → down, 学 study → learning, 間
  interval → space, 校 exam → school, 高 tall → high, 出 exit → leave, 小 little
  → small, 川 stream → river, 前 in front → before, 大 large → big, 中 in →
  middle, 万 ten thousand → 10,000, 話 tale → talk.
- Spanish (13): 学 estudio → aprendizaje, 間 intervalo → espacio, 見 vista →
  ver, 子 chiquillo → niño, 書 escritura → escribir, 女 femenino → mujer, 小
  poco → pequeño, 食 alimento → comer, 川 arroyo → río, 中 centro → dentro
  (override), 聞 preguntar → escuchar, 毎 siempre → cada, 話 cuento → hablar.

**JLPT N4** (50 English, 45 Spanish) and **N3** (163 English, 130 Spanish):
the report script lists every change.

**The original override table and the N2/N1/fr/pt additions** (what the first
meaning would be without it; the 12 N5-N4 lines are described above):

| kanji | lang | first | without | reason |
|---|---|---|---|---|
| 中 | es | dentro | centro | JLPT words favour "middle" (centro); keep "inside" (中に), as in the English list |
| 事 | en | matter | business | "business" comes from compounds (仕事); 事 is a thing or matter |
| 意 | en | idea | care | "care" comes from one compound (注意); 意 is idea, mind (意味, 意見) |
| 家 | en | house | family | "family" comes from compounds (家族); 家 alone is house, home |
| 屋 | en | shop | roof | KANJIDIC2 lists "roof" first; 屋 is the shop of 本屋, 花屋 (and 部屋) |
| 早 | en | early | fast | JMdict merges 早い (early) with 速い (fast) |
| 不 | en | negative | bad | "bad" comes from compounds (不便); 不 is the negative prefix |
| 転 | en | revolve | change | "change" comes from rare compounds; 転 turns, rolls (回転, 運転, 転ぶ) |
| 計 | es | medir | pensar | "plan" (計画) has no Spanish gloss but "pensar"; 計 measures (時計, 計算) |
| 図 | es | dibujo | planear | "plan" (planear) overtakes the drawing / map sense |
| 勉 | es | esforzarse | ejercitarse | "ejercitarse" relates to "exertion"; 勉 is effort (勉強) |
| 位 | en | rank | about | "about" is ぐらい (位); 位 is rank, grade |
| 機 | en | machine | airplane | "airplane" comes from 飛行機; 機 is machine, mechanism (機械) |
| 刻 | en | engrave | mince | JMdict lists "mince" first for 刻む; 刻 is carve, engrave (時刻: time) |
| 息 | en | breath | son | "son" comes from 息子; 息 alone is breath |
| 助 | en | help | rescue | JMdict lists "rescue" first for 助ける; 助 is help (手助け, 助手) |
| 申 | es | decir | tener el honor de | the English list has no "say"; 申す is the humble "say" |
| 過 | es | pasar | error | "error" (過ち) overtakes pass, exceed (過ぎる, 過去) |
| 御 | es | prefijo honorífico | manipular | the honorific prefix (御飯, 御手洗い) matches no English gloss |
| 構 | es | construir | poner | "poner" is matched to "stance" through a synonym; 構 is build, posture (構造) |
| 合 | es | adecuado | sentar bien | "sentar bien" is a narrow sense of "suit"; 合 fits, matches (合う) |
| 収 | en | income | store | "store" comes from 収める; 収 is income, obtain (収入) |
| 突 | en | sudden | collision | "collision" comes from 衝突; learners meet 突 in 突然 (sudden) |
| 組 | es | grupo | juntar | the English list has no "group"; 組 is a group, a class (組, 番組) |
| 確 | es | seguro | afirmar | "afirmar" (affirm) is matched to "firm"; 確 is certain, sure (確か) |
| 説 | es | explicar | teoría | "teoría" ties with "explicar" and comes first; 説 explains (説明) |
| 演 | es | representar | realizar | "realizar" relates to "performance"; 演 performs, acts (演技, 演劇) |
| 額 | en | amount; sum; forehead | framed picture | 額 is the amount of 金額 and the forehead; "framed picture" is 額縁 |
| 額 | es | cantidad de dinero; frente | marco de un cuadro | as the English: 金額 and the forehead, not 額縁 |
| 灯 | en | lamp; a light; light; counter for lights | a light | 灯 is a lamp; "a light" reads as a mass noun first |
| 灯 | es | lámpara; luz; contador para luces | luz | as the English: the lamp itself, not light in general |
| 泊 | es | alojarse; pasar la noche; hospedar | hospedar | 泊まる is "to stay overnight"; "alojarse" is the usual intransitive form |
| 版 | es | edición; impresión | impresión | 版 is the edition/plate of 出版, 初版; "impresión" is the printing |
| 営 | es | negocio; dirigir un negocio; campamento | campamento | 営 is business/management (営業, 経営); the camp sense is rare |
| 貨 | es | dinero; moneda; bienes; artículos | bienes | 貨 is money/goods (貨幣, 貨物); "bienes" alone is less common |
| 訓 | es | lección; lectura japonesa de un carácter | lectura japonesa de un carácter | "lección" is the kanji's basic meaning; the 訓読み sense is long and compound |
| 霞 | es | neblina; bruma; estar brumoso; estar nebuloso | estar brumoso | 霞 is haze/mist; the verb is 霞む |
| 施 | es | distribuir; dar limosnas; administrar primeros auxilios | dar limosnas | 施 is give/bestow/perform; almsgiving is one sense (施し) |
| 趣 | es | significado; sentido; apariencia; proceder | proceder | 趣 is purport/gist; "proceder" comes from a related form |
| 慎 | es | prudencia; precaverse; ser cuidadoso; discrección; ser discreto; modestia; refrenarse | precaverse | the kanji is the noun; the verb is 慎む |
| 坪 | es | unidad de superficie; área de dos tatami (36 pies cuadrados) | área de dos tatami (36 pies cuadrados) | the long gloss overflows; 坪 is a unit of area |
| 豆 | es | legumbres; habichuelas; prefijo diminutivo | habichuelas | 豆 is beans/peas, not just green beans |
| 概 | es | aproximadamente; generalmente; condición; situación | condición | 概ね is "approximately"; "condición" is a derived sense |
| 脚 | es | pie; pata; posición; contador de patas | pata | 脚 is the leg/foot; "pata" is an animal or furniture leg |
| 甚 | es | tremendamente; en extremo; intenso; grave | grave | 甚だ is "very/extremely"; "grave" is a derived sense |
| 隊 | es | grupo; partido; banda | partido | 隊 is a group/company; "partido" reads as a political party or match |
| 椎 | es | roble; mazo | mazo | 椎 is a tree (chinquapin/oak); the mallet is 槌 |
| 洞 | es | cavidad; hueco; excavación; prever | excavación | 洞 is a cavity/cave; "excavación" is a derived sense |
| 篤 | es | sincero; serio; genuino; grave (enfermedad) | serio | 篤 is sincere/kind; "serio" is a derived sense |
| 票 | es | voto; etiqueta; contador de papeletas | etiqueta | 票 is a ballot/vote; "etiqueta" is a minor sense |
| 繁 | es | exuberante; próspero; sobrecrecido; complejo; problemático; confuso | complejo | 繁 is luxuriant/thick; "complejo" is the 繁雑 sense |
| 安 | fr | pas cher; paisible; calme; tranquille | paisible | 安い is "cheap"; the reorder keeps "paisible" first |
| 計 | fr | mesure; planifier; projeter; calcul | planifier | 計 is measure (時計, 計算); "planifier" is 計画 |
| 帰 | fr | retour à la maison; arriver à; mener à; aboutir à | arriver à | 帰 is the return home (帰る); "arriver à" is a related form |
| 私 | fr | je; privé; moi | moi | 私 is "I" (わたし); "privé" is the 私的 sense |
| 計 | pt | medida; desenho; plano; esquema | plano | 計 is measure (時計, 計算); "plano" is 計画 |
| 帰 | pt | Retôrno a casa; chegar em; conduzir-se; resultar | chegar em | 帰 is the return home (帰る); "chegar em" is a related form |
| 私 | pt | Eu; particular; me | me | 私 is "I" (わたし); "particular" is the 私的 sense |
| 失 | pt | perda; erro; falta; desvantagem | erro | 失 is loss (失う); "erro" is the 過失 sense |

Of the original 27 rows, twenty put back KANJIDIC2's own first meaning where a
step goes wrong (the English signal for 事, 意, 家, 早, 不, 転, 位, 刻, 息, 助,
収; the Spanish matching for 図, 勉, 申, 過, 御, 構, 合, 組, 確); seven choose
another of KANJIDIC2's meanings (中 dentro, 屋 shop, 計 medir, 機 machine, 突
sudden, 説 explicar, 演 representar). The 12 N5-N4 lines are described above;
the 9 N2, 15 N1 and 8 fr/pt lines are in the table.

**Still doubtful** (no override; most are defensible):

- English: 中 "middle" (Spanish keeps "dentro"), 円 "round" (yen?), 帰
  "homecoming" (no "return" in KANJIDIC2's list), 暮 "livelihood", 様 "way",
  存 "be aware of", 居 "to be", 参 "going", 主 "chief", 料 "materials".
- Spanish: 好 gustar algo, 着 llegar, 去 dejar, 台 pedestal, 主 jefe, 収
  ingresar, 居 existir, 曲 curva, 学 aprendizaje; N2's 枚 (a 41-character
  counter gloss), 宇 "cielo" and 預 "dejar al cuidado de" are left as they
  are. The N1 review (2026-11-18) covered its 121 flags; the remaining ones
  are defensible (e.g. 堕 "cometer un desliz", 聡 "que aprende rápido", 惨
  "desastroso", 密 "en secreto", 廉 "sincero").

**Built decks.** Every default deck (N5-N1 and Jōyō, es, en, fr and pt, with
stroke order and `--common-readings`) builds with the flag, validates, and passes
`scripts/check_reading_distractors.py` and `scripts/check_meaning_distractors.py`
(no precomputed meaning distractor shows a meaning of the target, or has the
target's first among its meanings, checked against KANJIDIC2's full lists).
Without the flag the builder's packs are byte-identical to b747c89's (N5 and
N3, es and en, checked with `--strokes --common-readings`, build time aside).
With it, sizes barely move and a build takes about 2 s longer (the vocabulary
glosses and the dictionary).

## Integration

- **Builder**: `fetch --readings --meanings` and `build --meaning-order`;
  `data/meaning_overrides.tsv`; `scripts/meaning_order_report.py`,
  `scripts/check_meaning_distractors.py`; tests in `tests/test_meanings.py`.
- **Pack format**: unchanged (the meanings were already "most common first").
- **`decks.yml`** (prepared, not run): `fetch --readings --meanings`,
  `--meaning-order` on every build line, the meaning check after the reading
  check, and the release notes' description and attribution.
- **Credits**: docs/data-sources.md; `firmware/src/Credits.h` `kMeanings`
  ("FreeDict/WikDict (Wiktionary), CC BY-SA 3.0/4.0"), shown on Settings › Device
  › About as "Meanings:" / "Significados:" and as a sixth credit in the portal
  page footer. The learner signal's sources (JMdict, the JLPT lists) are
  already credited there.
- **Firmware**: nothing else needed (the quiz already shows the first meaning).

## Follow-ups

- German and Italian: KANJIDIC2 has no meanings in those languages; a licensed,
  reviewed base list is needed before decks can exist.
- French and Portuguese example-word glosses: no reviewed table yet, so their
  intro cards show the word and reading only (docs/example-words.md).
- A reverse (Spanish-English) dictionary would close gaps such as 下 debajo
  directly; WikDict's `es-en` exists but is not versioned.
