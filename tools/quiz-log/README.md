# TGC_QUIZ_LOG Hard-after analysis

`tamagotcho-pack` and the firmware have no analytics; the quiz trace is the
source of truth. `sticks3-dev` prints one line per question and answer over the
USB serial port:

```
Q mean mode=0 U+65E5 |持つ *valor |... 
A sel=2 correct=1 OK grade=good rt=2310ms S=0.40 D=5.0 I=1
```

`rt` is the response time from the question's first frame to the press (less
the per-tap navigation allowance), and `grade` is the Good/Hard/Again the
device chose at the time. `analyze.py` reads a capture and answers the question
the roadmap leaves open: **is the "Hard after" threshold right?**

## Capture

```sh
cd firmware
pio run -e sticks3-dev -t upload
pio device monitor > quiz.log
```

Use the device normally for a while (30+ correct answers give a steadier
picture). Keep the setting fixed during a capture: the tool infers it from the
logged grades and warns when the grades are inconsistent with any one setting
(the setting changed, or the log was edited). Stop the monitor with Ctrl-C.

The capture may also come from `tools/screenshots/remote.py` sessions or a
saved serial log; only the `Q` and `A` lines matter.

## Run

```sh
python3 tools/quiz-log/analyze.py quiz.log
# or from a pipe:
pio device monitor | python3 tools/quiz-log/analyze.py -
```

Example:

```
TGC_QUIZ_LOG Hard-after analysis
  answers: 412 (381 correct, 31 missed)
  correct response time:
    p50 1840 ms
    p75 2760 ms
    p90 3900 ms
    p95 5100 ms
    p99 8800 ms
    min 620 ms, max 21400 ms
  logged grades: good 300, hard 81, again 31
  inferred setting: 3 s (0 mismatch(es) with the logged grades)
  what each threshold would do to the correct answers:
    threshold   good   hard   hard share
    1.5 s        190    191    50.1 %
    3 s          300     81    21.3 %
    5 s          347     34     8.9 %
    off          381      0     0.0 %
  recommendation: 5 s
    the 85th percentile is 3900 ms; at 5 s 34 of 381 correct answers
    (8.9 %) are Hard
  modes:
    meaning          190 correct, median 1600 ms
    reading          120 correct, median 2100 ms
    meaning->kanji    71 correct, median 2300 ms
```

## Reading it

- **`--target-hard`** (default `0.15`) is the share of correct answers the
  tool allows to be Hard. It recommends the smallest of the device's choices
  (1.5 s, 3 s, 5 s, off) at or above the quantile, so the Hard share stays at
  or below the target. A lower target grades fewer answers Hard; a higher one
  grades more.
- **The inferred setting** is the choice whose Good/Hard split best matches
  the logged `grade` values. With 0 mismatches it is the setting the capture
  ran with. "unclear" means the grades fit no single choice (more than one
  answer in ten differs), usually because the setting changed mid-capture.
- **Per-mode medians** show where the time goes. If reading answers are much
  slower than meaning answers, a single threshold may over-grade readings; the
  fix is then a different threshold, not a per-mode setting (the firmware has
  one).
- **`--max-rt MS`** drops answers slower than `MS` before analysing (0 keeps
  all). Use it to remove an interruption that left the device open; it never
  changes the raw capture.
- **`--tsv PATH`** writes every parsed answer (`mode`, `kanji`, `step`,
  `correct`, `grade`, `rt_ms`) for a spreadsheet or a deeper look.

## Apply

Settings › Study › Hard after on the device offers 1.5 s, 3 s, 5 s and off
(the default is 3 s). Change it, keep capturing, and compare a later run; the
grade affects the SRS interval (docs/srs.md §3), so a change is worth a few
days of normal use before judging it. The tool only recommends; no firmware
default changes automatically.

## Tests

```sh
python3 -m unittest discover -s tools/quiz-log -v
```
