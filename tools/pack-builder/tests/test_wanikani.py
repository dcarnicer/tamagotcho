"""WaniKani importer tests.

Everything runs offline against the committed canned responses in
``tests/data/wanikani/`` (hand-written, not real account data). The default
transport is replaced by a fake, and a fixture fails the test if the real
network layer is ever reached.
"""

import io
import json
import urllib.error
import urllib.request
from pathlib import Path

import pytest

from tangocho_pack import cli, wanikani
from tangocho_pack.kanjidic import MAX_MEANINGS

DATA = Path(__file__).parent / "data" / "wanikani"
TOKEN = "test-token-not-real"

# Expected unlocked kanji, ordered by (WaniKani level, subject id).
NICHI, ICHI, NI, SAN, JU, GETSU, YAMA = 0x65E5, 0x4E00, 0x4E8C, 0x4E09, 0x5341, 0x6708, 0x5C71
EXPECTED = [ICHI, NI, NICHI, GETSU, YAMA]


class FakeTransport:
    """Serves the canned pages and records the URLs it was asked for."""

    def __init__(self):
        self.calls: list[str] = []

    def __call__(self, url: str, token: str) -> dict:
        self.calls.append(url)
        assert token == TOKEN
        if "subjects" in url:
            name = "subjects.json"
        elif "page_after_id" in url:
            name = "assignments_page2.json"
        else:
            name = "assignments_page1.json"
        return json.loads((DATA / name).read_text(encoding="utf-8"))


@pytest.fixture
def no_network(monkeypatch):
    """Any use of the real transport is a bug in a test."""
    def boom(*_a, **_k):
        raise AssertionError("tests must not touch the network")
    monkeypatch.setattr(wanikani, "fetch_json", boom)


def import_cards(no_network, **kw):
    fake = FakeTransport()
    cards = wanikani.wanikani_cards(TOKEN, transport=fake, sleep=lambda _s: None, **kw)
    return fake, cards


def test_unlocked_included_locked_and_hidden_excluded(no_network):
    fake, cards = import_cards(no_network)
    # Every request went through the fake transport (assert the network was not used).
    assert fake.calls and all("api.wanikani.com" in u for u in fake.calls)
    # 三 is locked (unlocked_at null), 十 is hidden, 452/453 are not kanji.
    assert [c.codepoint for c in cards] == EXPECTED
    assert SAN not in [c.codepoint for c in cards]
    assert JU not in [c.codepoint for c in cards]


def test_pagination_follows_next_url(no_network):
    fake, cards = import_cards(no_network)
    assert any("page_after_id=1003" in u for u in fake.calls)
    # 日/月/山 live on the second assignments page.
    assert [c.codepoint for c in cards] == EXPECTED
    assignments_calls = [u for u in fake.calls if "assignments" in u]
    assert len(assignments_calls) == 2


def test_meanings_primary_first_and_capped(no_network):
    _, cards = import_cards(no_network)
    nichi = next(c for c in cards if c.codepoint == NICHI)
    # primary "Sun" arrives after "Day" in the fixture but must lead; the
    # non-accepted "Japan" is the 4th and is dropped by the cap.
    assert nichi.meanings == ["Sun", "Day", "Sunday"]
    assert len(nichi.meanings) <= MAX_MEANINGS
    assert "Japan" not in nichi.meanings


def test_readings_split_on_kun_primary_first(no_network):
    _, cards = import_cards(no_network)
    nichi = next(c for c in cards if c.codepoint == NICHI)
    # on'yomi shifted to katakana, primary (じつ) first, duplicate にち removed.
    assert nichi.onyomi == ["ジツ", "ニチ"]
    # kun'yomi stay hiragana, primary first; nanori かず is not a reading.
    assert nichi.kunyomi == ["ひ", "か"]
    assert all(r != "かず" for r in nichi.onyomi + nichi.kunyomi)


def test_unknown_metadata_is_zero(no_network):
    _, cards = import_cards(no_network)
    for c in cards:
        assert (c.jlpt, c.grade, c.strokes, c.freq) == (0, 0, 0, 0)


def test_levels_and_ordering_are_deterministic(no_network):
    fake_a, a = import_cards(no_network)
    _, b = import_cards(no_network)
    assert [c.codepoint for c in a] == [c.codepoint for c in b] == EXPECTED
    assert [c.level for c in a] == list(range(len(a)))
    # 一 and 二 are level 1, then 日/月 level 2, then 山 level 3.
    assert [c.level for c in a] == sorted(c.level for c in a)


def test_level_range_limits_and_filters_locally(no_network):
    fake, cards = import_cards(no_network, levels={2})
    # The API filter is requested ...
    assert any("levels=2" in u for u in fake.calls)
    # ... and the local filter excludes the level-1 and level-3 kanji even
    # though the fixture transport ignores the query parameter.
    assert [c.codepoint for c in cards] == [NICHI, GETSU]
    assert all(c.level in (0, 1) for c in cards)


def test_other_meaning_language_falls_back_to_english(no_network):
    _, cards = import_cards(no_network, lang="es")
    assert cards
    for c in cards:
        assert c.fallback_en is True
        assert all(m.isascii() for m in c.meanings)


def test_english_meanings_have_no_fallback_flag(no_network):
    _, cards = import_cards(no_network)
    assert all(c.fallback_en is False for c in cards)


def test_empty_account_returns_no_cards(no_network):
    def empty_transport(url, token):
        assert token == TOKEN
        return {"object": "collection", "pages": {"next_url": None}, "data": []}
    cards = wanikani.wanikani_cards(TOKEN, transport=empty_transport, sleep=lambda _s: None)
    assert cards == []


def test_fetch_json_401_is_clear_and_token_free(monkeypatch):
    seen = {}

    def fake_urlopen(req, timeout=None):
        seen.update({k.lower(): v for k, v in req.headers.items()})
        raise urllib.error.HTTPError(req.full_url, 401, "Unauthorized", {}, io.BytesIO(b"{}"))

    monkeypatch.setattr(urllib.request, "urlopen", fake_urlopen)
    with pytest.raises(wanikani.WaniKaniError) as ei:
        wanikani.fetch_json(wanikani.assignments_url(), "SECRET-TOKEN")
    message = str(ei.value)
    assert "401" in message
    assert "SECRET-TOKEN" not in message
    assert seen["authorization"] == "Bearer SECRET-TOKEN"
    assert seen["wanikani-revision"] == wanikani.REVISION == "20170710"


@pytest.mark.parametrize("code,needle", [(401, "401"), (403, "403"), (429, "429")])
def test_http_errors_are_mapped(code, needle):
    err = wanikani._http_error(code)
    assert isinstance(err, wanikani.WaniKaniError)
    assert needle in str(err)


def test_cli_token_from_flag_is_not_logged(monkeypatch, capsys):
    monkeypatch.delenv("WANIKANI_TOKEN", raising=False)

    def boom(*_a, **_k):
        raise wanikani.WaniKaniError("WaniKani rejected the token (HTTP 401 Unauthorized)")

    monkeypatch.setattr(wanikani, "wanikani_cards", boom)
    rc = cli.main(["build", "--wanikani-token", "FLAG-SECRET", "-o", "out.tgc"])
    captured = capsys.readouterr()
    assert rc == 2
    assert "401" in captured.err
    assert "FLAG-SECRET" not in captured.out + captured.err


def test_cli_reads_token_from_env(monkeypatch, capsys):
    monkeypatch.setenv("WANIKANI_TOKEN", "env-secret")
    seen = {}

    def fake_cards(token, **_kw):
        seen["token"] = token
        return []

    monkeypatch.setattr(wanikani, "wanikani_cards", fake_cards)
    rc = cli.main(["build", "-o", "out.tgc"])
    captured = capsys.readouterr()
    assert rc == 1  # no unlocked kanji selected
    assert seen["token"] == "env-secret"
    assert "env-secret" not in captured.out + captured.err


def test_cli_levels_passed_through(monkeypatch):
    monkeypatch.setenv("WANIKANI_TOKEN", "env-secret")
    seen = {}

    def fake_cards(token, **kw):
        seen.update(kw)
        seen["token"] = token
        return []

    monkeypatch.setattr(wanikani, "wanikani_cards", fake_cards)
    cli.main(["build", "--wanikani-level", "2", "--wanikani-level", "3", "-o", "out.tgc"])
    assert seen["levels"] == {2, 3}
    assert seen["lang"] == "en"


def test_cli_missing_font_is_a_clear_error(monkeypatch, capsys, tmp_path):
    # Checked once cards are selected, so the CLI tests above need no font.
    monkeypatch.setenv("WANIKANI_TOKEN", "env-secret")
    # Any non-empty selection: build_pack opens the font before it reads a card.
    monkeypatch.setattr(wanikani, "wanikani_cards", lambda token, **kw: [object()])
    missing = tmp_path / "NoSuchFont.otf"
    rc = cli.main(["build", "--font", str(missing), "-o", str(tmp_path / "out.tgc")])
    assert rc == 2
    err = capsys.readouterr().err
    assert "NoSuchFont.otf not found" in err and "tangocho-pack fetch" in err
    assert "env-secret" not in err


def test_default_deck_name():
    assert wanikani.default_wanikani_name(None) == "WaniKani"
    assert wanikani.default_wanikani_name({2}) == "WaniKani L2"
    assert wanikani.default_wanikani_name({2, 3}) == "WaniKani L2-3"
    assert wanikani.default_wanikani_name({1, 2, 3}) == "WaniKani L1-3"
