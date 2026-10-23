"""tamagotcho-pack command line."""

from __future__ import annotations

import argparse
import hashlib
import os
import sys
from pathlib import Path

from . import paths
from .kanjidic import ORDERS


def cmd_fetch(a) -> int:
    from .fetch import fetch_all
    fetch_all(force=a.force, klee_one=a.klee_one, readings=a.readings, meanings=a.meanings)
    return 0


def _meaning_order(raw: list, lang: str, check_raw: list | None = None) -> list | None:
    """`build --meaning-order` (docs/meaning-order.md): English meanings in the
    learner order (JLPT vocabulary + JMdict glosses), Spanish ones in the order
    of the new English ones (FreeDict's dictionary), then the project's
    overrides; None after printing an error. fr/pt have no dictionary: they
    keep KANJIDIC2's order (the flag still reorders the English meanings their
    fallback cards show, and keeps meaning distractors away from the meanings
    the pack leaves out: cmd_build). `check_raw` is the full KANJIDIC2 list
    when `raw` is a selection (`--kanji-file`): overrides name kanji that may
    not be in the selection but must still exist in KANJIDIC2."""
    from .meanings import (LANGS, EnglishOrder, MeaningOrder, check_overrides, load_freedict,
                           load_learner_vocab, load_overrides, reorder_raw)
    from .readings import load_jlpt_vocab

    need = [paths.JMDICT_GZ, *paths.JLPT_VOCAB.values()]
    if lang in LANGS:
        need.append(paths.FREEDICT_ENG_SPA)
    missing = [p for p in need if not p.exists()]
    if missing:
        print("error: --meaning-order needs " + ", ".join(p.name for p in missing)
              + ": run `tamagotcho-pack fetch --readings --meanings`", file=sys.stderr)
        return None
    try:
        overrides = load_overrides(paths.MEANING_OVERRIDES)
        errors = check_overrides(overrides, check_raw if check_raw is not None else raw)
        if errors:
            raise ValueError(f"{paths.MEANING_OVERRIDES.name}: " + "; ".join(errors))
        english = EnglishOrder(load_learner_vocab(load_jlpt_vocab(paths.JLPT_VOCAB),
                                                  paths.JMDICT_GZ))
        spanish = MeaningOrder(load_freedict(paths.FREEDICT_ENG_SPA)) if lang in LANGS else None
    except (ValueError, OSError) as e:
        print(f"error: {e}", file=sys.stderr)
        return None
    if lang != "en" and lang not in LANGS:
        print(f"warning: --meaning-order has no dictionary for {lang!r}: its meanings keep "
              "KANJIDIC2's order (meaning distractors avoid every meaning of a card)",
              file=sys.stderr)
    return reorder_raw(raw, lang, english, spanish, overrides)[0]


def _common_readings(raw: list, word_data: tuple | None = None) -> tuple[list, object] | None:
    """`build --common-readings`: KANJIDIC2 readings reordered most common
    first (docs/common-readings.md), or None after printing an error.
    `word_data` (_word_data()) saves weighing the JMdict words twice when
    `--example-words` needs them too."""
    need = [paths.JMDICT_GZ, paths.JMDICT_FURIGANA, paths.WORDFREQ_JA, paths.TATOEBA_JPN,
            paths.JOYO_PDF, *paths.JLPT_VOCAB.values()]
    missing = [p for p in need if not p.exists()]
    if missing:
        print("error: --common-readings needs " + ", ".join(p.name for p in missing)
              + ": run `tamagotcho-pack fetch --readings`", file=sys.stderr)
        return None
    from .joyo_table import load_pdf
    from .readings import compute_scores, load_jlpt_vocab, reorder_raw

    try:
        joyo = load_pdf(paths.JOYO_PDF)
    except ValueError as e:
        print(f"error: {e}", file=sys.stderr)
        return None
    if word_data is not None:
        pairs, vocab = word_data
        scores = compute_scores(raw, None, paths.JMDICT_FURIGANA, jlpt_vocab=vocab, pairs=pairs)
    else:
        scores = compute_scores(raw, paths.JMDICT_GZ, paths.JMDICT_FURIGANA, paths.WORDFREQ_JA,
                                paths.TATOEBA_JPN, load_jlpt_vocab(paths.JLPT_VOCAB))
    return reorder_raw(raw, scores, joyo, drop=True)


def _word_inputs() -> list:
    """What `--example-words` reads (`fetch --readings`)."""
    return [paths.JMDICT_GZ, paths.JMDICT_FURIGANA, paths.WORDFREQ_JA, paths.TATOEBA_JPN,
            *paths.JLPT_VOCAB.values()]


def _word_data() -> tuple[list, dict]:
    """The JMdict words weighted for a learner, and the JLPT vocabulary:
    what `--common-readings` scores readings with and `--example-words` picks
    words from (readings.weighted_pairs)."""
    from .readings import load_jlpt_vocab, weighted_pairs

    vocab = load_jlpt_vocab(paths.JLPT_VOCAB)
    return weighted_pairs(paths.JMDICT_GZ, paths.WORDFREQ_JA, paths.TATOEBA_JPN, vocab), vocab


def _missing(files, flag: str, fetch: str) -> bool:
    """Prints an error and returns True when an input of `flag` is missing."""
    missing = [p for p in files if not p.exists()]
    if missing:
        print(f"error: {flag} needs " + ", ".join(p.name for p in missing)
              + f": run `tamagotcho-pack fetch {fetch}`", file=sys.stderr)
    return bool(missing)


def _candidates(cards: list, word_data: tuple) -> tuple[dict, dict]:
    """(candidate words per kanji and reading, JMdict_e's word info) for the
    cards (words.py)."""
    from .readings import parse_furigana
    from .words import load_word_info, word_candidates

    pairs, vocab = word_data
    cands = word_candidates(cards, pairs, parse_furigana(paths.JMDICT_FURIGANA), vocab)
    return cands, load_word_info(paths.JMDICT_GZ)


def _example_words(cards: list, lang: str, word_data: tuple, table_path: Path) -> list | None:
    """`build --example-words`: each card's words with the deck language's
    glosses (words.py), or None after printing an error."""
    from .words import example_words, load_gloss_table, pick_words, skip_words

    try:
        table = load_gloss_table(table_path)
    except ValueError as e:
        print(f"error: {e}", file=sys.stderr)
        return None
    cands, info = _candidates(cards, word_data)
    picks = pick_words(cards, cands, info, skip_words(table))
    words = example_words(picks, info, lang, table)
    kinds = sum(len(ps) for ps in picks)
    n = sum(len(ws) for ws in words)
    on = sum(1 for ws in words for w in ws if w.kind == 0)
    glossed = sum(1 for ws in words for w in ws if w.gloss)
    source = {"en": "JMdict_e", "es": f"the reviewed rows of {table_path.name}"}.get(lang)
    print(f"example words: {n} for {len(cards)} cards (on'yomi {on}, kun'yomi {n - on}; "
          f"{kinds - n} asked readings without a usable word); glosses for {glossed} of {n}"
          + (f" from {source}" if source else f" (no gloss source for {lang!r})"))
    if lang == "es" and not table:
        print(f"warning: {table_path} not found: Spanish example words have no gloss",
              file=sys.stderr)
    return words


def cmd_build(a) -> int:
    from .build import build_pack, default_deck_name, default_deck_name_from_file
    from .kanjivg import KanjiVGError
    from .pack import read_pack

    # The WaniKani path is opt-in: it needs the user's own token (flag or env)
    # instead of the cached KANJIDIC2 file. The token is never printed, logged
    # or written to cache/.
    token = a.wanikani_token or os.environ.get("WANIKANI_TOKEN")
    words = None
    if a.kanji_file and (a.jlpt or a.grade or a.joyo):
        print("error: --kanji-file selects the kanji itself; it cannot be combined with "
              "--jlpt, --grade or --joyo", file=sys.stderr)
        return 2
    if a.kanji_file and token:
        print("error: --kanji-file cannot be combined with --wanikani-token or "
              "WANIKANI_TOKEN", file=sys.stderr)
        return 2
    if a.order == "file" and not a.kanji_file:
        print("error: --order file is the --kanji-file line order; give --kanji-file",
              file=sys.stderr)
        return 2
    if token:
        if a.common_readings:
            print("error: --common-readings orders KANJIDIC2 readings; a WaniKani pack already "
                  "lists each kind's primary reading first", file=sys.stderr)
            return 2
        if a.meaning_order:
            print("error: --meaning-order orders KANJIDIC2 meanings; WaniKani's are English "
                  "only, in WaniKani's order", file=sys.stderr)
            return 2
        if a.example_words:
            print("error: --example-words picks words for KANJIDIC2 readings; it cannot be "
                  "combined with a WaniKani import", file=sys.stderr)
            return 2
        from .wanikani import WaniKaniError, default_wanikani_name, wanikani_cards

        levels = set(a.wanikani_level) if a.wanikani_level else None
        try:
            cards = wanikani_cards(token, lang=a.lang, levels=levels)
        except WaniKaniError as e:
            print(f"error: {e}", file=sys.stderr)
            return 2
        if not cards:
            print("error: no unlocked WaniKani kanji selected", file=sys.stderr)
            return 1
        name = a.name or default_wanikani_name(levels)
    else:
        from .kanjidic import (JOYO_GRADES, load_new_jlpt, make_cards, parse_kanji_file,
                               parse_kanjidic)

        jlpt, grades = set(a.jlpt or []), set(a.grade or [])
        if a.joyo and grades:
            print("error: --joyo already selects grades 1-6 and 8; drop --grade", file=sys.stderr)
            return 2
        if a.joyo:
            grades = set(JOYO_GRADES)
        if not jlpt and not grades and not a.kanji_file:
            print("error: give at least one --jlpt, --grade, --joyo or --kanji-file, or a "
                  "--wanikani-token", file=sys.stderr)
            return 2
        if not paths.KANJIDIC_GZ.exists():
            print("error: run `tamagotcho-pack fetch` first", file=sys.stderr)
            return 2
        new = None
        if paths.KANJI_JSON.exists():
            new = load_new_jlpt(paths.KANJI_JSON)
        else:
            print("warning: kanji.json missing, mapping old 4-level JLPT (lossy)", file=sys.stderr)
        # The order becomes the new-card order on the device: school order for
        # the Jōyō deck, N5-first for everything else unless --order says so,
        # and the file's line order for --kanji-file.
        order = a.order or ("file" if a.kanji_file else "grade" if a.joyo else "jlpt")
        all_raw = None
        if a.kanji_file:
            try:
                text = Path(a.kanji_file).read_text(encoding="utf-8-sig")
            except (OSError, UnicodeDecodeError) as e:
                print(f"error: {a.kanji_file}: {e}", file=sys.stderr)
                return 2
            all_raw = parse_kanjidic(paths.KANJIDIC_GZ)
            by_cp = {k.codepoint: k for k in all_raw}
            try:
                cps, warnings = parse_kanji_file(text, set(by_cp))
            except ValueError as e:
                print(f"error: {a.kanji_file}: {e}", file=sys.stderr)
                return 2
            for w in warnings:
                print(f"warning: {a.kanji_file}: {w}", file=sys.stderr)
            raw = [by_cp[cp] for cp in cps]
            if not raw:
                print(f"error: no kanji from {a.kanji_file} are in KANJIDIC2", file=sys.stderr)
                return 1
        else:
            raw = parse_kanjidic(paths.KANJIDIC_GZ)
        word_data = None
        if a.example_words:
            if _missing(_word_inputs(), "--example-words", "--readings"):
                return 2
            word_data = _word_data()
        before = None
        if a.common_readings:
            before = {k.codepoint: k for k in raw}
            got = _common_readings(raw, word_data)
            if got is None:
                return 2
            raw = got[0]
        from .meanings import LANGS as MEANING_LANGS
        reorder_meanings = a.meaning_order and (a.lang == "en" or a.lang in MEANING_LANGS)
        meanings_before = {k.codepoint: k.meanings.get(a.lang, []) for k in raw}
        if a.meaning_order:
            got = _meaning_order(raw, a.lang, check_raw=all_raw)
            if got is None:
                return 2
            raw = got
        # The pack keeps at most three meanings (no more than English has), and
        # the reordered list can keep other ones than KANJIDIC2's order: with
        # the flag, every meaning of a card keeps meaning distractors away, in
        # every language.
        cards = make_cards(raw, a.lang, jlpt, grades, new, order=order,
                           keep_all_meanings=a.meaning_order)
        if not cards:
            print("error: no cards selected", file=sys.stderr)
            return 1
        if before is not None:
            def changed(kind: str) -> tuple[int, int]:
                new = [(getattr(c, kind), getattr(before[c.codepoint], kind)) for c in cards]
                return (sum(1 for n, o in new if n and n[0] != o[0]),
                        sum(1 for n, o in new if o and not n))
            (on, on_drop), (kun, kun_drop) = changed("onyomi"), changed("kunyomi")
            print(f"common readings: first on'yomi changed for {on} and first kun'yomi for "
                  f"{kun} of {len(cards)} cards; on'yomi dropped for {on_drop}, kun'yomi "
                  f"for {kun_drop} (nothing worth asking)")
        if reorder_meanings:
            own = [(c.meanings, meanings_before[c.codepoint]) for c in cards if not c.fallback_en]
            first = sum(1 for n, o in own if n[0] != o[0])
            kept = sum(1 for n, o in own if set(n) != set(o[:len(n)]))
            print(f"meaning order ({a.lang}): first meaning changed for {first} of {len(cards)} "
                  f"cards; the pack keeps other meanings for {kept}")
        name = a.name or (default_deck_name_from_file(a.kanji_file) if a.kanji_file
                          else default_deck_name(jlpt, grades, joyo=a.joyo))
        if word_data is not None:
            words = _example_words(cards, a.lang, word_data, Path(a.es_glosses))
            if words is None:
                return 2
    if not name.isascii():
        print(f"warning: deck name {name!r} is not ASCII; the device draws the deck name "
              "in its built-in ASCII font, so it will not show correctly", file=sys.stderr)
    from .catalog import title_problem
    problem = title_problem(name)
    if problem:
        print(f"warning: deck name {name!r} cannot be a catalog title ({problem}); "
              "`catalog` will refuse this pack", file=sys.stderr)
    try:
        raw = build_pack(cards, a.font, a.lang, name, a.large_px, a.small_px, a.text_px, a.option_px,
                         strokes=a.strokes, strokes_px=a.strokes_px, words=words)
    except (KanjiVGError, ValueError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    except FileNotFoundError:
        # A cache filled before Noto Sans JP became the default has only Klee One.
        # Reported here, after the selection, so argument and token errors come first.
        if Path(a.font).is_file():
            raise
        print(f"error: font {a.font} not found (run `tamagotcho-pack fetch`, or pass --font)",
              file=sys.stderr)
        return 2
    out = Path(a.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(raw)
    p = read_pack(raw)
    fb = sum(1 for c in p.cards if c.flags & 1)
    stroked = sum(1 for s in p.strokes if s)
    stroke_note = f", strokes for {stroked}/{len(p.cards)} cards" if p.header["stroke_off"] else ""
    if p.header["words_off"]:
        stroke_note += f", {sum(len(w) for w in p.words)} example words (v3)"
    print(f"{out}: {len(p.cards)} cards ({fb} with English fallback), "
          f"{len(p.glyphs)} glyphs, {len(raw):,} bytes{stroke_note}")
    if p.errors:
        print("pack validation errors:", *p.errors[:10], sep="\n  ", file=sys.stderr)
        return 1
    return 0


def cmd_sync_glosses(a) -> int:
    """`sync-glosses`: append the picked words the Spanish table lacks as
    draft rows (a FreeDict suggestion, never JMdict's Spanish), touching no
    existing row, and report what still lacks a reviewed gloss per level."""
    from .kanjidic import JOYO_GRADES, load_new_jlpt, make_cards, parse_kanji_file, parse_kanjidic
    from .words import (KIND_NAME, GlossRow, append_rows, load_freedict_ordered,
                        load_gloss_table, pick_words, skip_words, translate)

    if bool(a.kanji_file) == bool(a.jlpt or a.joyo):
        print("error: give --jlpt N (repeatable) and/or --joyo, or --kanji-file", file=sys.stderr)
        return 2
    need = [paths.KANJIDIC_GZ, paths.KANJI_JSON, paths.JOYO_PDF, *_word_inputs()]
    if _missing(need + [paths.FREEDICT_ENG_SPA], "sync-glosses", "--readings --meanings"):
        return 2
    table_path = Path(a.es_glosses)
    try:
        table = load_gloss_table(table_path)
    except ValueError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    all_raw = parse_kanjidic(paths.KANJIDIC_GZ)
    new = load_new_jlpt(paths.KANJI_JSON)
    jlpt, grades = set(a.jlpt or []), set(JOYO_GRADES) if a.joyo else set()
    raw = all_raw
    if a.kanji_file:
        by_cp = {k.codepoint: k for k in all_raw}
        try:
            cps, warnings = parse_kanji_file(
                Path(a.kanji_file).read_text(encoding="utf-8-sig"), set(by_cp))
        except (OSError, UnicodeDecodeError, ValueError) as e:
            print(f"error: {a.kanji_file}: {e}", file=sys.stderr)
            return 2
        for w in warnings:
            print(f"warning: {a.kanji_file}: {w}", file=sys.stderr)
        raw = [by_cp[cp] for cp in cps]
    # The words are picked for the readings the published decks ask, so the
    # readings are ordered as `build --common-readings` orders them.
    word_data = _word_data()
    got = _common_readings(raw, word_data)
    if got is None:
        return 2
    if jlpt and a.joyo:
        # Each level's kanji, then the Joyo kanji with no JLPT level: the union.
        cards = make_cards(got[0], "en", jlpt, None, new, order="jlpt")
        seen = {c.codepoint for c in cards}
        cards += [c for c in make_cards(got[0], "en", None, grades, new, order="grade")
                  if c.codepoint not in seen]
    else:
        order = "file" if a.kanji_file else ("grade" if a.joyo else "jlpt")
        cards = make_cards(got[0], "en", jlpt, grades, new, order=order)
    if not cards:
        print("error: no cards selected", file=sys.stderr)
        return 1
    cands, info = _candidates(cards, word_data)
    picks = pick_words(cards, cands, info, skip_words(table))
    unskipped = pick_words(cards, cands, info)

    def level_of(c) -> str:
        return f"N{c.jlpt}" if c.jlpt else ("Joyo" if c.grade in JOYO_GRADES else "-")

    # Every picked word once, in card order, with the kanji that picked it.
    picked: dict[tuple[str, str], dict] = {}
    for c, ps in zip(cards, picks):
        for pk in ps:
            if pk.word is None:
                continue
            e = picked.setdefault((pk.word.text, pk.word.reading),
                                  {"level": level_of(c), "kanji": []})
            if c.char not in e["kanji"]:
                e["kanji"].append(c.char)
    fd = load_freedict_ordered(paths.FREEDICT_ENG_SPA)
    added: list[GlossRow] = []
    for key, e in picked.items():
        if key in table:
            continue
        i = info.get(key)
        eng = i.eng if i else []
        t = translate(eng, i.pos if i else (), fd)
        note = ("CHECK: FreeDict suggestion (sync-glosses), not reviewed" if t
                else "CHECK: no FreeDict suggestion (sync-glosses)")
        added.append(GlossRow(e["level"], ",".join(e["kanji"]), key[0], key[1],
                              "; ".join(eng[:3]), t[0] if t else "", "draft", note))
    if added and not a.dry_run:
        append_rows(table_path, added)
    after = {**table, **{r.key(): r for r in added}}

    # What the skip rows changed.
    for c, ps, us in zip(cards, picks, unskipped):
        for pk, uk in zip(ps, us):
            if uk.word is not None and (pk.word is None or (pk.word.text, pk.word.reading)
                                        != (uk.word.text, uk.word.reading)):
                now = f"{pk.word.text} {pk.word.reading}" if pk.word else "no word"
                print(f"skip: {c.char} {KIND_NAME[pk.kind]} {pk.asked}: {uk.word.text} "
                      f"{uk.word.reading} is skipped, picked {now}")
    verb = "would append" if a.dry_run else "appended"
    print(f"{table_path.name}: {verb} {len(added)} draft row(s)")
    for r in added[:a.show]:
        print(f"  {r.level}  {r.kanji}  {r.word}  {r.reading}  [{r.en}]  {r.es or '-'}")
    if len(added) > a.show:
        print(f"  ... {len(added) - a.show} more (--show N)")

    # Per level: the words and their state in the table.
    levels: dict[str, dict] = {}
    for c, ps in zip(cards, picks):
        lv = levels.setdefault(level_of(c), {"cards": 0, "asked": 0, "slots": 0, "words": {}})
        lv["cards"] += 1
        lv["asked"] += len(ps)
        for pk in ps:
            if pk.word is not None:
                lv["slots"] += 1
                lv["words"].setdefault((pk.word.text, pk.word.reading), c.char)
    print("level  cards  asked  words   ok  draft  no word")
    lacking = []
    for name in sorted(levels, key=lambda n: (n[0] != "N", -int(n[1:]) if n[0] == "N" else 0, n)):
        lv = levels[name]
        ws = lv["words"]
        ok = sum(1 for k in ws if after[k].status == "ok")
        print(f"{name:<6} {lv['cards']:>5}  {lv['asked']:>5}  {len(ws):>5}  {ok:>3}  "
              f"{len(ws) - ok:>5}  {lv['asked'] - lv['slots']:>7}")
        lacking += [(name, ch, k) for k, ch in ws.items() if after[k].status != "ok"]
    print("(words: distinct picked words; ok/draft: their rows; no word: asked readings "
          "without a usable word)")
    if a.list:
        for name, ch, (w, r) in lacking:
            print(f"  lacks an ok gloss: {name} {ch} {w} {r} ({after[(w, r)].status})")
    return 0


def cmd_inspect(a) -> int:
    from .pack import CARD_SIZE, GLYPH_SIZE, read_pack
    from .rle import raw1_size, raw4_size
    raw = Path(a.file).read_bytes()
    try:
        p = read_pack(raw)  # decodes and checks every glyph and stroke mask
    except ValueError as e:
        print(f"INVALID: {e}")
        return 1
    h = p.header
    sets = [sum(1 for g in p.glyphs if g.set == s) for s in (0, 1, 2, 3)]

    def ratio(stored: int, plain: int) -> str:
        return f"{stored / plain * 100:5.1f} %" if plain else "    - "

    print(f"file        {a.file}")
    print(f"sha256      {hashlib.sha256(raw).hexdigest()}")
    print(f"version     {h['version']}   lang {h['lang']}   deck {p.deck_name!r}")
    print(f"cards       {h['card_count']} ({sum(1 for c in p.cards if c.flags & 1)} English fallback)")
    print(f"glyphs      {h['glyph_count']} (set0 {sets[0]}, set1 {sets[1]}, set2 {sets[2]}, set3 {sets[3]})")
    print(f"px          large {h['large_px']}  small {h['small_px']}  text {h['text_px']} "
          f"(line {h['text_line_height']}, ascent {h['text_ascent']})  option {h['option_px']} "
          f"(line {h['option_line_height']}, ascent {h['option_ascent']})")
    glyph_raw = sum(raw4_size(g.width, g.height) for g in p.glyphs)
    glyph_stored = sum(g.size for g in p.glyphs)
    print("sections    bytes")
    print(f"  header          {h['header_size']:>9,}")
    print(f"  cards           {h['card_count'] * CARD_SIZE:>9,}")
    print(f"  glyph table     {h['glyph_count'] * GLYPH_SIZE:>9,}")
    print(f"  distractors     {h['strings_off'] - h['distract_off']:>9,}")
    print(f"  strings         {h['strings_size']:>9,}")
    print(f"  glyph bitmaps   {glyph_stored:>9,}")
    if h["stroke_off"]:
        stroked = sum(1 for s in p.strokes if s)
        strokes = sum(len(s) for s in p.strokes)
        end = h["words_off"] if h["words_off"] > h["stroke_off"] else len(raw)
        print(f"  strokes         {end - h['stroke_off']:>9,} "
              f"({stroked} cards, {strokes} masks)")
    if h["words_off"]:
        n = sum(len(w) for w in p.words)
        glossed = sum(1 for ws in p.words for w in ws if w.gloss)
        kanji = sum(1 for g in p.glyphs if g.set == 2 and g.codepoint >= 0x3400)
        print(f"  example words   {len(raw) - h['words_off']:>9,} "
              f"({n} words for {sum(1 for w in p.words if w)} cards, {glossed} with a gloss; "
              f"{kanji} kanji in set 2)")
    print(f"  total           {len(raw):>9,}")
    print("compression       raw    stored   ratio  (encoded)")
    for s, name in enumerate(("set 0", "set 1", "set 2", "set 3")):
        gl = [g for g in p.glyphs if g.set == s]
        if not gl:
            continue
        r = sum(raw4_size(g.width, g.height) for g in gl)
        st = sum(g.size for g in gl)
        print(f"  {name:<9} {r:>9,} {st:>9,} {ratio(st, r)}  ({sum(1 for g in gl if g.encoding)}"
              f"/{len(gl)} RLE)")
    print(f"  {'glyphs':<9} {glyph_raw:>9,} {glyph_stored:>9,} {ratio(glyph_stored, glyph_raw)}")
    masks = [m for s in p.strokes for m in s]
    if masks:
        r = sum(raw1_size(m.w, m.h) for m in masks)
        st = sum(m.size for m in masks)
        by = [sum(1 for m in masks if m.encoding == e) for e in (0, 1, 2)]
        print(f"  {'masks':<9} {r:>9,} {st:>9,} {ratio(st, r)}  "
              f"(raw {by[0]}, runs {by[1]}, row-xor runs {by[2]})")
    if p.errors:
        print(f"INVALID: {len(p.errors)} problem(s)")
        for e in p.errors[:50]:
            print("  -", e)
        return 1
    print("OK")
    return 0


def cmd_preview(a) -> int:
    from .pack import read_pack
    from .preview import render_preview
    if len(a.card) != 1:
        print("error: --card must be a single character", file=sys.stderr)
        return 2
    p = read_pack(Path(a.file).read_bytes())
    img, info = render_preview(p, ord(a.card), a.mode, a.width, a.height, a.scale, a.seed, a.reveal,
                               a.kind, a.stroke)
    img.save(a.output)
    if a.mode == "strokes":
        print(f"{a.output}: {info['stroke_count']} strokes")
    else:
        print(f"{a.output}: options {[chr(c) for c in info['options']]} "
              f"correct slot {info['correct_slot']}")
    if info.get("reading_kind"):
        print(f"  reading kind: {info['reading_kind']}")
    if info["missing"]:
        print("MISSING glyphs:", "".join(info["missing"]), file=sys.stderr)
        return 1
    return 0


def cmd_fixture(a) -> int:
    from .fixture import write_fixture
    for f in write_fixture(Path(a.font) if a.font else None):
        print(f"wrote {f} ({f.stat().st_size:,} bytes)")
    return 0


def cmd_catalog(a) -> int:
    from .catalog import render, render_json, scan

    directory = Path(a.directory)
    if not directory.is_dir():
        print(f"error: {directory} is not a directory", file=sys.stderr)
        return 1
    try:
        order = a.order.replace(",", " ").split() if a.order else None
        records = scan(directory, a.base_url, order)
    except (OSError, ValueError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    if not records:
        print(f"error: no *.tgc packs in {directory}", file=sys.stderr)
        return 1

    text = render(records)
    if a.output == "-":
        sys.stdout.write(text)
    else:
        out = Path(a.output)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(text, encoding="utf-8")
        print(f"{out}: {len(records)} pack(s)")
    if a.json:
        jp = Path(a.json)
        jp.parent.mkdir(parents=True, exist_ok=True)
        jp.write_text(render_json(records), encoding="utf-8")
        print(f"{jp}: {len(records)} pack(s), JSON")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="tamagotcho-pack")
    sub = ap.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("fetch", help="download KANJIDIC2, JLPT levels, Noto Sans JP and KanjiVG "
                                     "into cache/")
    s.add_argument("--force", action="store_true", help="download again even if present")
    s.add_argument("--klee-one", action="store_true",
                   help="also download Klee One (textbook-style font, the default before "
                        "2026-10-18) into cache/fonts/; build with it via "
                        "--font cache/fonts/KleeOne-SemiBold.ttf")
    s.add_argument("--readings", action="store_true",
                   help="also download what `build --common-readings` needs into "
                        "cache/readings/: JMdict, JmdictFurigana, wordfreq's Japanese list, "
                        "Tatoeba's Japanese indices and the official Jōyō table (PDF)")
    s.add_argument("--meanings", action="store_true",
                   help="also download what `build --meaning-order` needs into "
                        "cache/meanings/: FreeDict's English-Spanish dictionary (WikDict, "
                        "from Wiktionary; pinned release)")
    s.set_defaults(fn=cmd_fetch)

    s = sub.add_parser("build", help="build a .tgc pack (KANJIDIC2, a kanji list or your own "
                                     "WaniKani account)")
    s.add_argument("--kanji-file", metavar="PATH",
                   help="build from a UTF-8 text file with one kanji per non-blank line "
                        "(surrounding whitespace and # comments allowed); duplicates keep "
                        "their first occurrence, kanji missing from KANJIDIC2 are skipped "
                        "with a warning. The line order is the default new-card order; "
                        "--order jlpt|grade overrides it. The default deck name is the file "
                        "stem. Cannot be combined with --jlpt, --grade, --joyo or "
                        "--wanikani-token")
    s.add_argument("--jlpt", type=int, action="append", choices=range(1, 6), metavar="N")
    s.add_argument("--grade", type=int, action="append", choices=range(1, 11), metavar="N")
    s.add_argument("--joyo", action="store_true",
                   help="the 2,136 Jōyō kanji (KANJIDIC2 grades 1-6 and 8), in school order "
                        "(grade, then frequency); default name 'Joyo kanji'. AND-ed with --jlpt")
    s.add_argument("--order", choices=ORDERS,
                   help="order new cards are introduced in: jlpt (N5 first, then grade and "
                        "frequency), grade (school grade, then frequency) or file (the "
                        "--kanji-file line order). Default: file with --kanji-file, grade "
                        "with --joyo, otherwise jlpt")
    s.add_argument("--wanikani-token", metavar="TOKEN",
                   help="build from the kanji unlocked on your own WaniKani account; the token "
                        "is never logged or cached (the WANIKANI_TOKEN environment variable is "
                        "also read). Replaces the --jlpt/--grade (KANJIDIC2) selection and "
                        "cannot be combined with --kanji-file")
    s.add_argument("--wanikani-level", type=int, action="append", choices=range(1, 61), metavar="N",
                   help="limit the WaniKani import to this level (repeatable); "
                        "default: every unlocked level")
    s.add_argument("--lang", default="en", choices=["en", "es", "fr", "pt"])
    s.add_argument("--font", default=str(paths.DEFAULT_FONT),
                   help="TTF/OTF to render the glyphs from (default: Noto Sans JP Regular, "
                        "from `fetch`)")
    s.add_argument("--large-px", type=int, default=96)
    s.add_argument("--small-px", type=int, default=40)
    s.add_argument("--text-px", type=int, default=18)
    s.add_argument("--option-px", type=int, default=20,
                   help="size of set 3 (large text for reading answers); 0 to omit")
    s.add_argument("--strokes", action="store_true",
                   help="include KanjiVG stroke-order masks (needs `tamagotcho-pack fetch`)")
    s.add_argument("--strokes-px", type=int,
                   help="em square for stroke masks; must equal --large-px (the default)")
    s.add_argument("--common-readings", action="store_true",
                   help="order each kanji's on'yomi and kun'yomi with the Jōyō table's "
                        "approved readings first, then the most common first (word "
                        "frequencies), so the device asks a common reading; every reading is "
                        "kept. Needs `fetch --readings` (docs/common-readings.md)")
    s.add_argument("--meaning-order", action="store_true",
                   help="put each kanji's most useful meaning first (the device shows the "
                        "first): English meanings by the JLPT vocabulary words that use the "
                        "kanji (their JMdict glosses, easier levels first), Spanish ones in the "
                        "order of the English ones they translate (an English-Spanish "
                        "dictionary), then a short hand-checked override table. No meaning is "
                        "added or removed; fr/pt keep KANJIDIC2's order. In every language, "
                        "meaning distractors also avoid the meanings the pack leaves out. Needs "
                        "`fetch --readings --meanings` (docs/meaning-order.md)")
    s.add_argument("--example-words", action="store_true",
                   help="add example words (pack format v3): per card, a common word for its "
                        "asked on'yomi and one for its asked kun'yomi, with the reading and a "
                        "gloss in the deck's language (English from JMdict_e, Spanish only from "
                        "the reviewed rows of --es-glosses; none otherwise), and their kanji "
                        "in glyph set 2. Firmware without v3 support refuses the pack. Needs "
                        "`fetch --readings`; meant with --common-readings, which orders the "
                        "readings the device asks")
    s.add_argument("--es-glosses", default=str(paths.EXAMPLE_GLOSSES_ES), metavar="PATH",
                   help="the Spanish example-gloss table (default: data/example_glosses_es.tsv); "
                        "its skip rows apply in every language")
    s.add_argument("--name", help="deck name (default derived from filters)")
    s.add_argument("-o", "--output", required=True)
    s.set_defaults(fn=cmd_build)

    s = sub.add_parser("sync-glosses",
                       help="append the example words a deck picks and the Spanish gloss table "
                            "lacks, as draft rows, and report what lacks a reviewed gloss")
    s.add_argument("--jlpt", type=int, action="append", choices=range(1, 6), metavar="N",
                   help="a JLPT level's deck (repeatable)")
    s.add_argument("--joyo", action="store_true", help="the Joyo deck")
    s.add_argument("--kanji-file", metavar="PATH", help="a deck from a kanji list (as `build`)")
    s.add_argument("--es-glosses", default=str(paths.EXAMPLE_GLOSSES_ES), metavar="PATH",
                   help="the table to fill (default: data/example_glosses_es.tsv; created "
                        "when missing)")
    s.add_argument("--dry-run", action="store_true", help="report only; write nothing")
    s.add_argument("--list", action="store_true",
                   help="also list every picked word that lacks an ok gloss")
    s.add_argument("--show", type=int, default=50, metavar="N",
                   help="appended rows to print (default 50)")
    s.set_defaults(fn=cmd_sync_glosses)

    s = sub.add_parser("inspect", help="validate a pack (decoding every bitmap) and print a summary")
    s.add_argument("file")
    s.set_defaults(fn=cmd_inspect)

    s = sub.add_parser("preview", help="render a simulated device screen from a pack")
    s.add_argument("file")
    s.add_argument("--card", required=True)
    s.add_argument("--mode", choices=["meaning", "reading", "kanji", "strokes"], default="meaning")
    s.add_argument("--kind", choices=["on", "kun"], default="on",
                   help="reading kind to ask for in reading mode")
    s.add_argument("--stroke", type=int, default=-1,
                   help="strokes mode: 0-based last stroke to draw highlighted, -1 = all")
    s.add_argument("--width", type=int, default=135)
    s.add_argument("--height", type=int, default=240)
    s.add_argument("--scale", type=int, default=3)
    s.add_argument("--seed", type=int, default=1)
    s.add_argument("--reveal", action="store_true", help="highlight the correct option")
    s.add_argument("-o", "--output", required=True)
    s.set_defaults(fn=cmd_preview)

    s = sub.add_parser("fixture", help="regenerate fixtures/mini.tgc, mini.json and rle_vectors.bin")
    s.add_argument("--font")
    s.set_defaults(fn=cmd_fixture)

    from .catalog import FILE_NAME as CATALOG_FILE, JSON_FILE_NAME as CATALOG_JSON
    from .catalog import VERSION as CATALOG_VERSION
    s = sub.add_parser("catalog", help=f"write the line-oriented catalog (format v{CATALOG_VERSION}) "
                                       "of .tgc packs for the device")
    s.add_argument("directory", nargs="?", default=".", help="directory to scan for *.tgc (default: .)")
    s.add_argument("--base-url",
                   help="prefix for the url column; may contain {file}, otherwise joined with '/'")
    s.add_argument("--json", metavar="PATH",
                   help="also write a human-readable JSON catalog (not read by the device; "
                        f"published as {CATALOG_JSON})")
    s.add_argument("--order", metavar="NAMES",
                   help="pack names (without .tgc, comma or space separated) to list first, in "
                        "this order; the rest follow by file name")
    s.add_argument("-o", "--output", required=True,
                   help=f"catalog file to write (published as {CATALOG_FILE}), or - for stdout")
    s.set_defaults(fn=cmd_catalog)

    a = ap.parse_args(argv)
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
