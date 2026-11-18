#!/usr/bin/env python3
"""Summarise a TGC_QUIZ_LOG capture and recommend a "Hard after" threshold.

The firmware's `sticks3-dev` build prints one `A ...` line per answer:

    A sel=2 correct=1 OK grade=good rt=2310ms S=... D=... I=...

This tool pairs each answer with the preceding `Q ...` line (for the quiz
mode), reports the response-time distribution of correct answers and what each
of the device's thresholds (1.5 s, 3 s, 5 s, off) would have done, and picks
the smallest threshold that grades at most `--target-hard` of the correct
answers Hard. It also infers the threshold the device was using from the logged
grades, when the capture is consistent.

Stdlib only; run with `python3`. The log format is in firmware/README.md and
`docs/ux-refresh.md` §2.6.
"""
from __future__ import annotations

import argparse
import math
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

# The firmware's four Hard-after choices (ui/Choice.cpp); 0xFFFF disables Hard.
THRESHOLDS_MS = (1500, 3000, 5000, 0xFFFF)

MODE_NAMES = {0: "meaning", 1: "reading", 2: "meaning->kanji"}

Q_RE = re.compile(
    r"\bQ\s+\S+\s+mode=(\d+)\s+U\+([0-9A-Fa-f]+)(?:\s+step=(\d+))?"
)
A_RE = re.compile(
    r"\bA\s+sel=(-?\d+)\s+correct=([01])\s+(?:OK|MISS)\s+grade=(\w+)\s+rt=(\d+)ms"
)


@dataclass
class Answer:
    mode: int | None
    kanji: str | None
    step: int | None
    correct: bool
    grade: str
    rt_ms: int


@dataclass
class Capture:
    answers: list[Answer] = field(default_factory=list)
    unpaired: int = 0


def parse_capture(text: str) -> Capture:
    """Every `A` line, with the last `Q` line's mode and kanji."""
    out = Capture()
    mode = kanji = step = None
    for line in text.splitlines():
        q = Q_RE.search(line)
        if q:
            mode = int(q.group(1))
            kanji = "U+" + q.group(2).upper()
            step = int(q.group(3)) if q.group(3) is not None else None
            continue
        a = A_RE.search(line)
        if not a:
            continue
        if mode is None:
            out.unpaired += 1
            continue
        out.answers.append(
            Answer(
                mode=mode,
                kanji=kanji,
                step=step,
                correct=a.group(2) == "1",
                grade=a.group(3).lower(),
                rt_ms=int(a.group(4)),
            )
        )
    return out


def percentile(sorted_values: list[int], pct: float) -> int | None:
    """Nearest-rank percentile, pct in 0..100 (85 -> the slowest 15 % start)."""
    if not sorted_values:
        return None
    pct = max(0.0, min(100.0, pct))
    k = max(0, math.ceil(pct / 100.0 * len(sorted_values)) - 1)
    return sorted_values[min(k, len(sorted_values) - 1)]


def hard_share(answers: list[Answer], threshold_ms: int) -> tuple[int, float]:
    """Correct answers graded Hard at `threshold_ms` (0xFFFF = off)."""
    correct = [a for a in answers if a.correct]
    if not correct:
        return 0, 0.0
    if threshold_ms == 0xFFFF:
        return 0, 0.0
    n = sum(1 for a in correct if a.rt_ms >= threshold_ms)
    return n, n / len(correct)


def infer_threshold(answers: list[Answer]) -> tuple[int | None, int]:
    """The choice whose Hard/Good split best matches the logged grades.

    Returns (threshold, mismatches); threshold is None when the answer set is
    too small or no choice is consistent enough. A correct answer is Hard at a
    threshold iff its rt is at least that threshold.
    """
    correct = [a for a in answers if a.correct]
    if len(correct) < 2:
        return None, len(correct)
    best: tuple[int, int] | None = None
    for t in THRESHOLDS_MS:
        mismatches = sum(
            1 for a in correct
            if ((a.rt_ms >= t) and (t != 0xFFFF)) != (a.grade == "hard")
        )
        if best is None or mismatches < best[1]:
            best = (t, mismatches)
    assert best is not None
    # More than one answer in ten differing means the setting probably changed
    # during the capture (or the log was edited): do not claim a setting.
    if best[1] > max(1, len(correct) // 10):
        return None, best[1]
    return best


def recommend(answers: list[Answer], target_hard: float) -> tuple[int, int | None]:
    """The smallest threshold at or above the (1 - target) quantile.

    With target_hard 0.15, at most ~15 % of the correct answers are Hard. When
    even 5 s leaves more, 5 s is recommended with a warning; the caller prints
    the table.
    """
    correct = sorted(a.rt_ms for a in answers if a.correct)
    if not correct:
        return 0xFFFF, None
    q = percentile(correct, (1.0 - target_hard) * 100.0)
    for t in THRESHOLDS_MS:
        if t != 0xFFFF and q is not None and t >= q:
            return t, q
    return 5000, q


def fmt_threshold(ms: int) -> str:
    return "off" if ms == 0xFFFF else f"{ms / 1000:g} s"


def print_report(capture: Capture, target_hard: float, stream=None) -> None:
    if stream is None:
        stream = sys.stdout
    answers = capture.answers
    correct = [a for a in answers if a.correct]
    missed = [a for a in answers if not a.correct]
    print("TGC_QUIZ_LOG Hard-after analysis", file=stream)
    print(f"  answers: {len(answers)} ({len(correct)} correct, {len(missed)} missed)", file=stream)
    if capture.unpaired:
        print(f"  ignored: {capture.unpaired} answer line(s) with no Q line", file=stream)
    if not correct:
        print("  no correct answers: nothing to calibrate", file=stream)
        return

    rts = sorted(a.rt_ms for a in correct)
    print("  correct response time:", file=stream)
    for label, pct in (("p50", 50), ("p75", 75), ("p90", 90), ("p95", 95), ("p99", 99)):
        print(f"    {label} {percentile(rts, pct)} ms", file=stream)
    print(f"    min {rts[0]} ms, max {rts[-1]} ms", file=stream)

    grades = {g: sum(1 for a in correct if a.grade == g) for g in ("good", "hard")}
    print(f"  logged grades: good {grades['good']}, hard {grades['hard']}, "
          f"again {len(missed)}", file=stream)
    inferred, mismatches = infer_threshold(answers)
    if inferred is not None:
        print(f"  inferred setting: {fmt_threshold(inferred)} "
              f"({mismatches} mismatch(es) with the logged grades)", file=stream)
    else:
        print(f"  inferred setting: unclear ({mismatches} mismatch(es)); "
              "the threshold may have changed during the capture", file=stream)

    print("  what each threshold would do to the correct answers:", file=stream)
    print("    threshold   good   hard   hard share", file=stream)
    for t in THRESHOLDS_MS:
        n, share = hard_share(answers, t)
        print(f"    {fmt_threshold(t):<10} {len(correct) - n:>5}  {n:>5}   "
              f"{share * 100:>5.1f} %", file=stream)

    rec, q = recommend(answers, target_hard)
    n, share = hard_share(answers, rec)
    print(f"  recommendation: {fmt_threshold(rec)}", file=stream)
    if q is not None:
        print(f"    the {100 * (1.0 - target_hard):g}th percentile is {q} ms; "
              f"at {fmt_threshold(rec)} {n} of {len(correct)} correct answers "
              f"({share * 100:.1f} %) are Hard", file=stream)
    if rec == 5000 and q is not None and q > 5000:
        print("    the sample is slower than 5 s at that percentile: consider "
              "\"off\" if Hard is not useful", file=stream)
    if len(correct) < 30:
        print(f"    note: only {len(correct)} correct answers; 30 or more give "
              "a steadier picture", file=stream)
    print("  modes:", file=stream)
    for mode, name in MODE_NAMES.items():
        subset = [a for a in correct if a.mode == mode]
        if subset:
            mid = percentile(sorted(a.rt_ms for a in subset), 50)
            print(f"    {name:<15} {len(subset):>4} correct, median {mid} ms", file=stream)


def write_tsv(capture: Capture, path: Path) -> None:
    with path.open("w", encoding="utf-8", newline="\n") as f:
        f.write("mode\tkanji\tstep\tcorrect\tgrade\trt_ms\n")
        for a in capture.answers:
            f.write(f"{MODE_NAMES.get(a.mode, a.mode)}\t{a.kanji or ''}\t"
                    f"{a.step if a.step is not None else ''}\t{int(a.correct)}\t"
                    f"{a.grade}\t{a.rt_ms}\n")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("log", nargs="?", default="-",
                        help="the captured log (default: stdin; '-' is stdin)")
    parser.add_argument("--target-hard", type=float, default=0.15, metavar="SHARE",
                        help="the most correct answers to grade Hard (default: 0.15)")
    parser.add_argument("--max-rt", type=int, default=0, metavar="MS",
                        help="drop answers slower than MS (0: keep all)")
    parser.add_argument("--tsv", type=Path, metavar="PATH",
                        help="write the parsed answers as a TSV")
    args = parser.parse_args(argv)
    if not 0.0 <= args.target_hard <= 1.0:
        parser.error("--target-hard must be between 0 and 1")

    text = sys.stdin.read() if args.log == "-" else Path(args.log).read_text(
        encoding="utf-8", errors="replace")
    capture = parse_capture(text)
    if args.max_rt > 0:
        before = len(capture.answers)
        capture.answers = [a for a in capture.answers if a.rt_ms <= args.max_rt]
        dropped = before - len(capture.answers)
        if dropped:
            print(f"dropped {dropped} answer(s) slower than {args.max_rt} ms",
                  file=sys.stderr)
    if not capture.answers:
        print("no answer lines found: is this a TGC_QUIZ_LOG capture?", file=sys.stderr)
        return 1
    print_report(capture, args.target_hard)
    if args.tsv:
        write_tsv(capture, args.tsv)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
