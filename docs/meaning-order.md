# Spanish meanings in the English order

Status (2026-10-20): researched and implemented on the branch
`feature/es-meanings` as `tangocho-pack build --meaning-order`, following the
maintainer's decision to **order the Spanish meanings by the English order**.
`decks.yml` is prepared to use it for the published decks but has not been
run: the maintainer reviews the N5 changes and the cases to double-check
first. Without the flag the builder's output is byte-identical to before.

## Problem

KANJIDIC2's English meanings are listed most common first, but its Spanish
list often is not, and the device shows the first meaning: 持 es "duración;
propiedad; cargo; sostener; mantener" against en "hold; have", 界 "borde;
frontera; extensión; ámbito" against "world; boundary", 題 "objeto; tema;
materia" against "topic; subject". The pack spec already says meanings are
"most common first"; for Spanish the builder broke that.

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
  (`MAX_MEANINGS`). Spanish lists have 3.5 meanings on average, so the cut
  matters: 持 kept "duración; propiedad", the two glosses that translate
  neither "hold" nor "have".
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

So reordering inside the list changes what the device shows with no format or
firmware change. Because of the caps it also changes **which** meanings a
Spanish card stores (607 of 2,310 kanji): the reordered list keeps the glosses
that translate the English ones.

## Sources compared

Packs are redistributed under CC BY-SA 4.0 in the public `decks-v2` release.
A pack built with the flag contains only KANJIDIC2's own Spanish meanings in
another order, but the order is derived from the source, so it must allow
that use and is credited. No machine translation service or language model is
used: the build is reproducible and offline once the cache is filled.

| source | what it gives | licence | size / pinning | verdict |
|---|---|---|---|---|
| **FreeDict eng-spa** (WikDict) | 64,258 English headwords with their Spanish translations, per sense, from Wiktionary via DBnary | **CC BY-SA 3.0** (the TEI header and `COPYING`); a later version (4.0) is allowed for adaptations (§4(b)) | 3.7 MB `.src.tar.xz`, a dated release (`2025.11.23`) with a published checksum | **used** |
| WikDict SQLite (`en-es`, `es-en`, `en-fr`, `en-pt`) | the same data, newer, both directions and French/Portuguese | CC BY-SA ("Data from Wiktionary via DBnary") | 6-24 MB each, one unversioned URL regenerated in place (last 2026-06-23) | not used: cannot be pinned. Only used to estimate fr/pt (below) |
| kaikki.org (wiktextract) | English Wiktionary, Spanish entries with English glosses, translation tables | Wiktionary's (CC BY-SA, GFDL) | 1.05 GB for the Spanish file, 24 GB raw; regenerated weekly at fixed URLs, Wayback copies truncated | not used: size and no pinning; FreeDict/WikDict is a curated subset of the same data |
| **full multilingual JMdict** (`JMdict.gz`) | 68,452 Spanish glosses on 34,310 entries, next to the English ones (word-level co-occurrence: 持つ "to hold" / "sostener") | **unclear**: the EDRDG licence covers "the Japanese and English components (the translational equivalents in other languages, e.g. German, French, Dutch, etc. are covered by separate copyright held by the compilers of that material)" | 22 MB, daily, not pinned | **not usable**. Prototyped: it matched 281 more glosses, mostly right, but its senses are not aligned with the English ones (聞く hear / preguntar) |
| Apertium `apertium-en-es` | English-Spanish bilingual dictionary | GPL-2.0 | — | not usable (GPL is not compatible with CC BY-SA 4.0 output) |
| FreeDict spa-eng 0.3.1 | 4,502 Spanish headwords | not checked (an old hand-made edition) | small | not needed: tiny next to eng-spa |
| normalisation alone | same word in both lists (hospital, China) and cognates (institución / institution) | — | — | **used** inside the method; alone it matches 15 % of the glosses (table below) |

URLs: FreeDict database (release, checksum) https://freedict.org/freedict-database.json,
download https://download.freedict.org/dictionaries/eng-spa/2025.11.23/ ·
WikDict https://www.wikdict.com/page/about, files https://download.wikdict.com/dictionaries/sqlite/2/ ·
DBnary licence https://kaiko.getalp.org/about-dbnary/ ("Dbnary is derived from
Wiktionary and is distributed under Creative Commons Attribution-ShareAlike
3.0") · CC BY-SA 3.0 §4(b) https://creativecommons.org/licenses/by-sa/3.0/legalcode ·
kaikki.org https://kaikki.org/dictionary/rawdata.html · EDRDG licence
https://www.edrdg.org/edrdg/licence.html · Apertium https://github.com/apertium/apertium-en-es.

### The FreeDict dictionary: source and licence chain

- **Wiktionary** (CC BY-SA, GFDL) → **DBnary** (Gilles Sérasset, extracts the
  translations; dataset CC BY-SA 3.0) → **WikDict** (Karl Bartel, builds the
  bilingual dictionaries, CC BY-SA) → **FreeDict** release `eng-spa
  2025.11.23` ("English-español FreeDict+WikDict dictionary", maintainer Karl
  Bartel; TEI `availability`: "Licensed under the Creative Commons
  Attribution-ShareAlike 3.0 Unported license"; the archive ships the licence
  as `COPYING`).
- CC BY-SA 3.0 §4(b) lets an adaptation be distributed under "a later version
  of this License with the same License Elements", so it fits the packs'
  CC BY-SA 4.0, as KanjiVG's CC BY-SA 3.0 already does.
- `fetch --meanings` downloads the source archive (`.src.tar.xz`, the TEI
  inside) and checks its SHA256; the builder reads the TEI from the archive.

## Method (`tangocho_pack/meanings.py`)

1. **Normalisation**, both languages: lower case, parentheses removed
   (KANJIDIC2 splits a few glosses inside them: "llevar (en coche", "etc.)"),
   accents ignored, no leading "to"/"to be"/"be" or Spanish article; a
   Spanish gloss is also tried without a reflexive `-se` (levantarse) and
   without a trailing preposition (debajo de), an English one by its lemma when
   the dictionary lacks the form (going -> go).
2. **Translation**: the Spanish gloss is the English one (hospital, China) or
   one of its FreeDict translations.
3. **Related form**, only when the gloss translates none of the English glosses
   up to that point: a word sharing a long stem with a translation (préstamo /
   prestar for "lend", inteligente / inteligencia, celebración / celebrar), a
   cognate (matrimonio / matrimony, Japón / Japan: five letters or more, the
   first three shared, at most 40 % edited), the head words of a multi-word
   gloss (pájaro grande / large bird, tinta / black ink), or a translation of a
   near-synonym, an English word sharing translations with it (debajo
   translates "under", which shares bajo and abajo with "below").
4. **Order**: each Spanish gloss scores the index of the first English gloss it
   matches; a related form counts **one position later** (it ties with a
   translation of the next English gloss, and a translation of the same English
   gloss comes first: 貸 prestar before préstamo). Sort by score; **ties keep
   KANJIDIC2's order**; **unmatched glosses go last**, in KANJIDIC2's order.
5. Then the builder's existing caps apply (as many as English has, at most
   three). Nothing is added, removed or rewritten.

**The invariant.** With the flag, `Card.valid_meanings` holds every KANJIDIC2
meaning of the card in its language (before the caps), and
`meaning_distractors` also drops a candidate whose first meaning is one of the
target's meanings, or whose meanings include the target's first, stored or
not. The flag does this **in every language**: KANJIDIC2 has meanings beyond
the cap in English too, and default packs break the invariant through them
(N3 en: 取's "take" is offered for 受, whose fourth meaning is "take"; 資's
"assets" for 財, whose fourth is "assets"; 9 findings in N3 es). Without the
flag nothing changes. The firmware checks the stored meanings only, so the
unstored ones matter only when the precomputed list (12 cards) runs out and the
device fills the options from a random walk.

### Choices tested

Coverage of each step over the 8,124 Spanish glosses of the 2,310 kanji with
both languages (JLPT N5-N1 ∪ Jōyō):

| matching | glosses matched | kanji with nothing matched | first meaning changes | of them N5 |
|---|---:|---:|---:|---:|
| normalisation only (same word) | 125 (2 %) | 2,193 | 72 | 5 |
| + cognates | 1,202 (15 %) | 1,433 | 452 | 9 |
| FreeDict translations (with normalisation) | 4,276 (53 %) | 303 | 601 | 9 |
| + related forms (chosen) | 5,457 (67 %) | 89 | 602 | 8 |

Translations alone moved 下's "inferior" (the sixth English gloss) before
"debajo": FreeDict lists "debajo de" under "under", not under "below". The
synonym step brings it back.

- **Tie-breaks.** Within one English gloss, two alternatives to KANJIDIC2's
  order were tried: the dictionary's sense order (136 first meanings differ),
  and "translates more of the English glosses first" (215 differ). Both fix
  some cards (明 claro before inteligente, 小 pequeño, 天 cielo, 力 poder) and
  break others (sense order: 青 verde before azul, 着 llegar, 動 movimiento, 旅
  trayecto, 局 departamento; coverage also 時 hora, 名 reputación, 夜 tarde, 足
  pata, 風 aire). By eye over N5-N3, sense order comes out about even and
  coverage clearly worse. Ties keep KANJIDIC2's order, as decided; sense order
  is the alternative if 明 matters more than 青.
- **Related forms one position later.** Without the penalty (122 first
  meanings differ) a related form ties with the exact translation and often
  comes first: 貸 préstamo before prestar, 教 enseñanza before enseñar, 歩 camino
  before caminar, 難 dificultad before difícil, 悲 tristeza before triste.
  Ranking every related form after every translation instead loses the good
  ones (永 eterno, 叡 inteligente, 姻 matrimonio would go behind a later gloss).
- **No learner signal.** The roadmap item suggested the JLPT vocabulary signal
  used for readings; with the English order as the reference nothing else is
  needed to rank the glosses, and the cases left are dictionary gaps, not
  frequency questions.
- **Cognates even next to the dictionary's own translations**: restricting
  them to English words the dictionary lacks changed 8 first meanings, 7 for
  the worse (姻 matrimonio, 改 reforma, 宗 enseñanzas religiosas, 礼 saludo would
  lose their place). The one false friend seen, 雅 gracioso (funny) for
  "gracious", ties with elegante (a translation of "elegant") and stays behind
  it.

### French and Portuguese

`decks.yml` accepts `fr` and `pt`. FreeDict has no WikDict English-French or
English-Portuguese release (its `eng-fra` 0.1.6 and `eng-por` 0.3 are old,
small, hand-made editions), and JMdict has no Portuguese glosses (and French
ones of the same unclear status). So **Spanish only**; with the flag, `fr`/`pt`
keep KANJIDIC2's order (a warning says so) and get the stricter distractor
exclusion. An estimate with WikDict's unversioned SQLite files (translations
only, no related forms) says they would change about as much: the first
meaning of 619 of 2,034 French and 471 of 1,944 Portuguese lists (N5: 月 lune
→ mois, 人 être humain → personne, 車 véhicule → voiture; 高 Caro → alto, 川
corrente → rio). A follow-up if those decks matter (docs/roadmap.md).

## Results

`uv run python scripts/meaning_order_report.py` (after `fetch --meanings`).
2,383 kanji (JLPT N5-N1 ∪ Jōyō), 2,310 with Spanish and English meanings.

- Glosses: 8,124; by translation 4,044 (50 %), by a related form 1,413 (stem
  495, cognate 159, head words 506, synonym 253); unmatched 2,667 (33 %, mostly
  KANJIDIC2's extra Spanish senses: 生 vivir, existir, crudo; 上 cima,
  superficie). 89 kanji have no gloss matched and keep their order.
- **First meaning changed: 602**; stored meanings changed: 607.

| | N5 | N4 | N3 | N2 | N1 | no level |
|---|---:|---:|---:|---:|---:|---:|
| kanji | 79 | 166 | 367 | 367 | 1,232 | 172 |
| first meaning changed | 8 | 38 | 97 | 87 | 339 | 33 |
| stored meanings changed | 14 | 40 | 132 | 85 | 312 | 24 |

**Sanity table.** Superscript: the English gloss a Spanish gloss translates
(1 = first); `~n`: a related form of English gloss n (it counts one position
later); none: unmatched. *Stored*: what the pack keeps (= unchanged).

| kanji | English | stored before | stored after | full order |
|---|---|---|---|---|
| 持 | hold; have | duración; propiedad | sostener; mantener | sostener<sup>1</sup> · mantener<sup>1</sup> · duración · propiedad · cargo |
| 界 | world; boundary | borde; frontera | frontera; borde | frontera<sup>2</sup> · borde · extensión · ámbito |
| 題 | topic; subject | objeto; tema | tema; materia | tema<sup>1</sup> · materia<sup>~1</sup> · objeto |
| 院 | Inst.; institution; temple; mansion; school | institución; templo; hospital | = | institución<sup>2</sup> · templo<sup>3</sup> · hospital |
| 安 | relax; cheap; low; quiet; rested; contented; peaceful | barato; tranquilo; pacífico | = | barato<sup>2</sup> · tranquilo<sup>4</sup> · pacífico<sup>7</sup> |
| 重 | heavy; important; esteem; respect; heap up; pile up; nest of boxes; -fold | pesado; importante; apilar | = | pesado<sup>1</sup> · importante<sup>2</sup> · apilar<sup>6</sup> · amontonar<sup>~5</sup> |
| 生 | life; genuine; birth | vida; nacimiento; vivir | vida; nacimiento; dar a luz | vida<sup>1</sup> · nacimiento<sup>3</sup> · dar a luz<sup>3</sup> · vivir · existir · nacer · puro · crudo |
| 行 | going; journey; carry out; conduct; act; line; row; bank | ir; fila; línea (texto) | ir; realizar; llevar a cabo | ir<sup>1</sup> · realizar<sup>3</sup> · llevar a cabo<sup>3</sup> · conducta<sup>~3</sup> · ocurrir<sup>~3</sup> · fila<sup>6</sup> · línea (texto)<sup>6</sup> · ocurrencia |
| 明 | bright; light | luz; brillo | inteligente; claro | inteligente<sup>1</sup> · claro<sup>1</sup> · luminoso<sup>1</sup> · luz<sup>2</sup> · brillo<sup>~1</sup> · iluminar<sup>2</sup> · claridad · listo · aclarar |
| 気 | spirit; mind; air; atmosphere; mood | espíritu; mente | = | espíritu<sup>1</sup> · mente<sup>2</sup> |
| 日 | day; sun; Japan; counter for days | día; solar; Japón | día; sol; Japón | día<sup>1</sup> · sol<sup>2</sup> · Japón<sup>3</sup> · solar · rayo de sol |
| 上 | above; up | arriba; encima | = | arriba<sup>1</sup> · encima<sup>1</sup> · parte superior · cima · superficie |
| 下 | below; down; descend; give; low; inferior | debajo; inferior; debajo de | debajo; debajo de; inferior | debajo<sup>~1</sup> · debajo de<sup>~1</sup> · inferior<sup>6</sup> · parte inferior<sup>~6</sup> |
| 大 | large; big | grande; muy | = | grande<sup>1</sup> · muy · mucho |
| 人 | person | persona | = | persona<sup>1</sup> |
| 中 | in; inside; middle; mean; center | centro; dentro; interior | dentro; interior; centro | dentro<sup>1</sup> · interior<sup>~1</sup> · centro<sup>3</sup> · medio<sup>3</sup> |
| 女 | woman; female | femenino; mujer | mujer; femenino | mujer<sup>1</sup> · femenino<sup>2</sup> |
| 子 | child; sign of the rat; 11PM-1AM; first sign of Chinese zodiac | chiquillo; niño | niño; chiquillo | niño<sup>1</sup> · chiquillo |
| 書 | write | escritura | escribir | escribir<sup>1</sup> · escritura · escrito |
| 毎 | every | siempre | cada | cada<sup>1</sup> · siempre · en ese momento |
| 聞 | hear; ask; listen | preguntar; escuchar; rumor | escuchar; oír; preguntar | escuchar<sup>1</sup> · oír<sup>1</sup> · preguntar<sup>2</sup> · rumor · reputación |
| 見 | see; hopes; chances; idea; opinion; look at; visible | vista; ver; esperanza | ver; esperanza; idea | ver<sup>1</sup> · esperanza<sup>2</sup> · idea<sup>4</sup> · opinión<sup>5</sup> · poder verse<sup>~6</sup> · vista · observar · ser visto · estar a la vista · mostrar |
| 食 | eat; food | alimento; comida | comer; alimento | comer<sup>1</sup> · alimento<sup>2</sup> · comida<sup>2</sup> · eclipse |
| 小 | little; small | poco; pequeño | = | poco<sup>1</sup> · pequeño<sup>1</sup> |
| 天 | heavens; sky; imperial | paraíso; cielo; imperial | = | paraíso<sup>1</sup> · cielo<sup>1</sup> · imperial<sup>3</sup> |
| 先 | before; ahead; previous; future; precedence | antes; adelante; previo | = | antes<sup>1</sup> · adelante<sup>~1</sup> · previo<sup>3</sup> · anterior<sup>3</sup> · precedencia<sup>5</sup> · extremo · punta · final |
| 半 | half; middle; odd number; semi-; part- | mitad; medio; incompleto | mitad; medio; semi- | mitad<sup>1</sup> · medio<sup>1</sup> · semi-<sup>1</sup> · semi<sup>1</sup> · en parte<sup>~5</sup> · incompleto · a medias |
| 後 | behind; back; later | después; atrás; detrás | = | después<sup>1</sup> · atrás<sup>1</sup> · detrás<sup>1</sup> · retrasarse |
| 前 | in front; before | antes; delante de | = | antes<sup>2</sup> · delante de<sup>~2</sup> |

**JLPT N5, every first-meaning change** (79 cards; KANJIDIC2 → `--meaning-order`):
食 alimento → comer (eat; food), 中 centro → dentro (in; inside; middle), 子
chiquillo → niño (child), 書 escritura → escribir (write), 女 femenino → mujer
(woman; female), 聞 preguntar → escuchar (hear; ask; listen), 毎 siempre → cada
(every), 見 vista → ver (see). Nine more N5 cards keep their first meaning but
store other ones: 学 ciencia → estudiar, 行 fila; línea (texto) → realizar;
llevar a cabo, 高 sobresaliente → elevar, 生 vivir → dar a luz, 日 solar → sol,
半 incompleto → semi-, 本 base → contador de cosas alargadas, 名 célebre →
reputación, 話 charla → hablar.

**To double-check** (the report lists them per level): a change is doubtful
when the new first meaning matches only through a related form, or when the
old first matched nothing (a gap in the dictionary?) and the new one translates
a later English gloss. Most are right (界 frontera, 堂 salón, 文 literatura, 座
asiento, 最 más, 探 buscar, 遅 tarde, 寄 acercarse, 晴 despejarse, 相 mutuamente,
判 juzgar). The ones that look wrong or worse, N4-N3:

- 明 luz → **inteligente** (bright; light): "inteligente" translates a later
  sense of "bright"; claro and luminoso tie with it and come after it in
  KANJIDIC2's list. The worst N4 case.
- 御 prefijo honorífico → **manipular** (honorable; manipulate; govern): the
  honorific prefix matches nothing.
- 構 construir → **poner** (posture; stance; appearance; build): poner
  translates a near-synonym of "stance"; construir translates "build".
- 演 expresar → **realizar** (performance; act; play): realizar and
  representar both relate to translations of "performance" (realización,
  representación), and KANJIDIC2 lists realizar first.
- 確 seguro → **asegurarse** (assurance; firm): seguro matches nothing;
  asegurarse shares a stem with "aseguranza" (assurance).
- 勉 esforzarse → **ejercitarse** (exertion; endeavour; encourage; strive):
  esforzarse matches nothing; ejercitarse relates to "exertion".
- 組 grupo → **juntar** (association; braid; plait; construct; assemble): grupo
  matches nothing.
- Following the English order literally: 更 renovar → caer la noche (grow
  late), 申 decir → tener el honor de, 系 sistema → linaje (lineage; system).

**Built decks.** Every default deck (N5-N1 and Jōyō, es and en, with stroke
order and `--common-readings`) builds with the flag, validates, and passes
`scripts/check_reading_distractors.py` and `scripts/check_meaning_distractors.py`
(no precomputed meaning distractor shows a meaning of the target, or has the
target's first among its meanings, checked against KANJIDIC2's full lists;
the same check fails on the default N3 packs: 9 findings in es, 14 in en).
Without the flag the builder's packs are byte-identical to b747c89's (N5 and
N3, es and en, checked with `--strokes --common-readings`, build time
aside). With it, sizes barely move (N5 es 105,104 → 105,116 bytes, Jōyō es
3.0 MB) and a build takes about 1 s longer (loading the dictionary). In N3,
185 Spanish cards change their stored meanings (order or set); the
meaning-distractor lists change for 13 Spanish cards and, through the stricter
exclusion alone, for 14 English ones.

## Integration

- **Builder**: `fetch --meanings` (3.7 MB into `cache/meanings/`) and
  `build --meaning-order`; `scripts/meaning_order_report.py`,
  `scripts/check_meaning_distractors.py`; tests in `tests/test_meanings.py`.
- **Pack format**: unchanged (the meanings were already "most common first").
- **`decks.yml`** (prepared, not run): `fetch --readings --meanings`,
  `--meaning-order` on every build line, the meaning check after the reading
  check, and the release notes' description and attribution.
- **Credits**: docs/data-sources.md; `firmware/src/Credits.h` `kMeanings`
  ("FreeDict/WikDict (Wiktionary), CC BY-SA 3.0"), shown on Settings › Device
  › About as "Meanings:" / "Significados:" and as a sixth credit in the portal
  page footer ("meaning order: …").
- **Firmware**: nothing else needed (the quiz already shows the first
  meaning).

## Follow-ups

- French and Portuguese, if those decks are published: needs a pinned
  English-French/Portuguese dictionary under a compatible licence (a dated
  WikDict release on FreeDict would do).
- A reverse (Spanish-English) dictionary would close gaps such as 下 debajo
  directly; WikDict's `es-en` exists but is not versioned.
- The doubtful cases above, e.g. by letting a few hand-checked pairs override
  the order (a small committed table would need its own licence note).
