"""Tests for the TGC_QUIZ_LOG Hard-after analysis (stdlib unittest)."""
from __future__ import annotations

import io
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

import analyze


LOG = """\
Q mean mode=0 U+65E5
A sel=0 correct=1 OK grade=good rt=800ms S=0.40 D=5.0 I=1
Q read-on mode=1 U+6708
A sel=1 correct=1 OK grade=hard rt=4000ms S=0.40 D=5.0 I=1
Q k2m mode=2 U+5C71
A sel=0 correct=0 MISS grade=again rt=2500ms S=0.40 D=5.0 I=1
Q mean mode=0 U+6C34
A sel=2 correct=1 OK grade=good rt=1200ms S=0.40 D=5.0 I=1
Q read-kun mode=1 U+5DDD
A sel=1 correct=1 OK grade=good rt=2000ms S=0.40 D=5.0 I=1
"""


class ParseTests(unittest.TestCase):
    def test_pairs_answers_with_their_question(self):
        cap = analyze.parse_capture(LOG)
        self.assertEqual(len(cap.answers), 5)
        self.assertEqual(cap.unpaired, 0)
        self.assertEqual([a.mode for a in cap.answers], [0, 1, 2, 0, 1])
        self.assertEqual([a.kanji for a in cap.answers],
                         ["U+65E5", "U+6708", "U+5C71", "U+6C34", "U+5DDD"])
        self.assertEqual([a.rt_ms for a in cap.answers], [800, 4000, 2500, 1200, 2000])
        self.assertEqual([a.correct for a in cap.answers], [True, True, False, True, True])
        self.assertEqual([a.grade for a in cap.answers],
                         ["good", "hard", "again", "good", "good"])

    def test_an_answer_without_a_question_is_counted(self):
        cap = analyze.parse_capture("A sel=0 correct=1 OK grade=good rt=900ms\n")
        self.assertEqual(cap.answers, [])
        self.assertEqual(cap.unpaired, 1)


class StatsTests(unittest.TestCase):
    def setUp(self):
        self.correct = [
            analyze.Answer(0, "U+65E5", None, True, "good", 800),
            analyze.Answer(1, "U+6708", None, True, "good", 2500),
            analyze.Answer(0, "U+6C34", None, True, "hard", 3200),
            analyze.Answer(2, "U+5C71", None, True, "hard", 4800),
        ]

    def test_percentile_uses_nearest_rank(self):
        self.assertEqual(analyze.percentile([800, 1200, 2500, 4000], 50), 1200)
        self.assertEqual(analyze.percentile([800, 1200, 2500, 4000], 85), 4000)
        self.assertIsNone(analyze.percentile([], 50))

    def test_hard_share(self):
        self.assertEqual(analyze.hard_share(self.correct, 1500), (3, 0.75))
        self.assertEqual(analyze.hard_share(self.correct, 3000), (2, 0.5))
        self.assertEqual(analyze.hard_share(self.correct, 5000), (0, 0.0))
        self.assertEqual(analyze.hard_share(self.correct, 0xFFFF), (0, 0.0))

    def test_consistent_grades_infer_the_setting(self):
        threshold, mismatches = analyze.infer_threshold(self.correct)
        self.assertEqual((threshold, mismatches), (3000, 0))

    def test_changed_setting_is_not_inferred(self):
        mixed = self.correct + [
            analyze.Answer(0, "U+706B", None, True, "good", 9000),
            analyze.Answer(0, "U+6728", None, True, "good", 8000),
        ]
        threshold, mismatches = analyze.infer_threshold(mixed)
        self.assertIsNone(threshold)
        self.assertGreater(mismatches, 0)

    def test_recommendation_is_the_first_choice_above_the_quantile(self):
        # 85th percentile is 3200 ms: 5000 is the first choice at or above it.
        self.assertEqual(analyze.recommend(self.correct, 0.15)[0], 5000)
        # A faster sample recommends the 1.5 s choice.
        fast = [analyze.Answer(0, "U+65E5", None, True, "good", 700),
                analyze.Answer(0, "U+6C34", None, True, "good", 1200)]
        self.assertEqual(analyze.recommend(fast, 0.15)[0], 1500)


class ReportTests(unittest.TestCase):
    def test_report_names_the_recommendation_and_modes(self):
        cap = analyze.parse_capture(LOG)
        out = io.StringIO()
        with redirect_stdout(out):
            analyze.print_report(cap, 0.15)
        text = out.getvalue()
        self.assertIn("answers: 5 (4 correct, 1 missed)", text)
        self.assertIn("inferred setting: 3 s", text)
        self.assertIn("recommendation: 5 s", text)
        self.assertIn("reading", text)
        self.assertIn("1.5 s", text)

    def test_tsv_round_trip(self):
        cap = analyze.parse_capture(LOG)
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "answers.tsv"
            analyze.write_tsv(cap, p)
            lines = p.read_text(encoding="utf-8").splitlines()
        self.assertEqual(len(lines), 6)
        self.assertEqual(lines[0], "mode\tkanji\tstep\tcorrect\tgrade\trt_ms")
        self.assertIn("meaning\tU+65E5", lines[1])


if __name__ == "__main__":
    unittest.main()
