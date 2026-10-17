"""Meaning-distractor ranking, including the shared-radical signal.

Meanings are ranked same JLPT level first, then a shared classical radical,
then visual similarity, then stroke-count distance. These tests pin each part
of that key with hand-built cards (no font needed).
"""

from tangocho_pack.distractors import meaning_distractors
from tangocho_pack.kanjidic import Card


def _order(cards, visual, i=0, **kw):
    return meaning_distractors(cards, visual, **kw)[i]


def test_shared_radical_outranks_equally_similar_at_same_level():
    cards = [Card(1, ["target"], [], [], jlpt=5, strokes=4, radicals=[75]),
             Card(2, ["shared"], [], [], jlpt=5, strokes=4, radicals=[75]),
             Card(3, ["other"], [], [], jlpt=5, strokes=4, radicals=[85])]
    # identical ink vectors -> equal visual similarity, so the radical decides.
    visual = [(1.0, 0.0), (1.0, 0.0), (1.0, 0.0)]
    assert _order(cards, visual) == [1, 2]


def test_shared_radical_beats_lower_similarity_within_a_level():
    # The point of the feature: the radical family wins over the noisier ink
    # cosine for otherwise plausible (same-level) candidates.
    cards = [Card(1, ["target"], [], [], jlpt=5, strokes=4, radicals=[75]),
             Card(2, ["shared"], [], [], jlpt=5, strokes=6, radicals=[75]),
             Card(3, ["lookalike"], [], [], jlpt=5, strokes=4, radicals=[85])]
    visual = [(1.0, 0.0), (0.2, 0.98), (1.0, 0.0)]  # card 2 is less similar
    assert _order(cards, visual)[0] == 1


def test_same_level_still_outranks_shared_radical():
    # Different-level ordering must not be broken by radicals: a same-level
    # candidate with no shared radical comes before a shared one a level off.
    cards = [Card(1, ["target"], [], [], jlpt=5, strokes=4, radicals=[75]),
             Card(2, ["same-level"], [], [], jlpt=5, strokes=4, radicals=[85]),
             Card(3, ["other-level"], [], [], jlpt=4, strokes=4, radicals=[75])]
    visual = [(1.0, 0.0), (1.0, 0.0), (1.0, 0.0)]
    assert _order(cards, visual) == [1, 2]


def test_gloss_overlap_excludes_even_when_radical_shared():
    cards = [Card(1, ["a"], [], [], jlpt=5, strokes=1, radicals=[75]),
             Card(2, ["a"], [], [], jlpt=5, strokes=1, radicals=[75]),  # gloss + radical
             Card(3, ["b"], [], [], jlpt=5, strokes=1, radicals=[85])]
    visual = [(1.0, 0.0), (1.0, 0.0), (1.0, 0.0)]
    order = _order(cards, visual)
    assert 1 not in order and order == [2]


def test_no_radical_data_matches_pure_similarity_order():
    # With no radical information the key degrades to the pre-radical ordering
    # (same level, then most similar, then closest stroke count).
    cards = [Card(1, ["target"], [], [], jlpt=5, strokes=4),
             Card(2, ["closest-look"], [], [], jlpt=5, strokes=9),
             Card(3, ["farther"], [], [], jlpt=5, strokes=4)]
    visual = [(1.0, 0.0), (1.0, 0.0), (0.0, 1.0)]
    assert _order(cards, visual) == [1, 2]


def test_deterministic_for_a_seed():
    cards = [Card(1, ["target"], [], [], jlpt=5, strokes=4, radicals=[75]),
             Card(2, ["a"], [], [], jlpt=5, strokes=4, radicals=[86]),
             Card(3, ["b"], [], [], jlpt=5, strokes=4, radicals=[85]),
             Card(4, ["c"], [], [], jlpt=5, strokes=4, radicals=[75])]
    visual = [(1.0, 0.0)] * 4  # everything ties but for the radical and the seed
    a = _order(cards, visual, seed=7)
    b = _order(cards, visual, seed=7)
    assert a == b
    assert a[0] == 3  # the shared-radical candidate leads


def test_list_is_capped_best_first():
    cards = [Card(1, ["target"], [], [], jlpt=5, strokes=1, radicals=[75])]
    for i in range(2, 20):
        cards.append(Card(i, [f"m{i}"], [], [], jlpt=5, strokes=1, radicals=[75]))
    # decreasing cosine similarity to the target, so the best-first order is fixed
    visual = [(1.0, 0.0)]
    for j in range(1, len(cards)):
        s = 1.0 / (j + 1)
        visual.append((s, (1.0 - s * s) ** 0.5))
    order = _order(cards, visual, k=3)
    assert order == [1, 2, 3]
