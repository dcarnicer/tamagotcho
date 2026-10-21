"""Optional WaniKani API v2 importer.

Builds cards from the kanji a user has **unlocked** on WaniKani, using the
user's own personal API token. Nothing from WaniKani is shipped with the repo
and nothing is fetched unless the user runs the importer explicitly. The token
is used only to sign the requests: it is never logged, printed, written to
``cache/`` or committed.

The API is documented at https://docs.api.wanikani.com/ (revision 20170710).
See ``docs/data-sources.md`` for the licence/trademark position and
``docs/decisions.md`` for why this is a separate, opt-in importer.

Design notes:

- The network layer is a plain ``transport(url, token) -> dict`` callable, so
  tests inject a canned transport and never touch the network. ``fetch_json``
  is the default (real) transport.
- Meanings come back English-only from WaniKani. Asking for another language
  falls back to English **and** sets the card's fallback flag, matching the
  KANJIDIC2 importer's behaviour.
- Readings come back in hiragana; on'yomi are shifted to katakana so the pack
  contract (``docs/pack-format.md``: on'yomi katakana, kun'yomi hiragana) holds.
"""

from __future__ import annotations

import json
import sys
import time
import urllib.error
import urllib.request
from collections.abc import Callable, Iterable

from .kanjidic import MAX_MEANINGS, Card

API_BASE = "https://api.wanikani.com/v2/"
# The required `Wanikani-Revision` header. 20170710 is the current (and so far
# only) public revision: https://docs.api.wanikani.com/20170710/
REVISION = "20170710"
# The API throttles to 60 requests per minute; requests are spaced accordingly.
REQUESTS_PER_MINUTE = 60
USER_AGENT = "tamagotcho-pack"
# WaniKani meanings are English. A request for another language falls back to
# English with the card's fallback flag set (docs/decisions.md).
MEANING_LANGS = frozenset({"en"})
# Hiragana -> katakana block offset (ぁ..ゖ -> ァ..ヶ).
_KATAKANA_SHIFT = 0x60
_HIRAGANA = range(0x3041, 0x3097)

# A transport takes (url, token) and returns the parsed JSON body.
Transport = Callable[[str, str], dict]
Sleep = Callable[[float], None]


class WaniKaniError(Exception):
    """A clear, token-free error from the WaniKani importer."""


def _http_error(code: int) -> WaniKaniError:
    if code == 401:
        return WaniKaniError(
            "WaniKani rejected the token (HTTP 401 Unauthorized); check the API token"
        )
    if code == 403:
        return WaniKaniError(
            "WaniKani refused the request (HTTP 403 Forbidden); the token may lack "
            "the required scopes"
        )
    if code == 429:
        return WaniKaniError(
            "WaniKani rate limit exceeded (HTTP 429); wait a minute and retry"
        )
    return WaniKaniError(f"WaniKani API error (HTTP {code})")


def fetch_json(url: str, token: str) -> dict:
    """Default transport: GET ``url`` with the user's token.

    The token is sent in the ``Authorization`` header only; it is never logged
    and never included in an error message.
    """
    req = urllib.request.Request(
        url,
        headers={
            "Authorization": f"Bearer {token}",
            "Wanikani-Revision": REVISION,
            "User-Agent": USER_AGENT,
        },
    )
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            return json.loads(r.read())
    except urllib.error.HTTPError as e:
        raise _http_error(e.code) from None
    except urllib.error.URLError as e:
        raise WaniKaniError(f"could not reach the WaniKani API: {e.reason}") from None


class _Pacer:
    """Space requests out to stay under the per-minute rate limit."""

    def __init__(self, sleep: Sleep, per_minute: int = REQUESTS_PER_MINUTE) -> None:
        self._sleep = sleep
        self._interval = 60.0 / per_minute
        self._next = 0.0

    def wait(self) -> None:
        now = time.monotonic()
        if now < self._next:
            self._sleep(self._next - now)
        self._next = max(now, self._next) + self._interval


def assignments_url(levels: Iterable[int] | None = None) -> str:
    """The assignments collection, ask for unlocked, non-hidden kanji."""
    params = ["subject_types=kanji", "unlocked=true", "hidden=false"]
    if levels is not None:
        params.append("levels=" + ",".join(str(n) for n in sorted(levels)))
    return API_BASE + "assignments?" + "&".join(params)


def subjects_url() -> str:
    """The subjects collection, kanji only (the join source for the data)."""
    return API_BASE + "subjects?types=kanji"


def collect_pages(
    url: str,
    token: str,
    transport: Transport,
    sleep: Sleep,
) -> list[dict]:
    """Follow ``pages.next_url`` and concatenate every ``data`` array."""
    pacer = _Pacer(sleep)
    out: list[dict] = []
    while url:
        pacer.wait()
        page = transport(url, token)
        out.extend(page.get("data") or [])
        url = (page.get("pages") or {}).get("next_url") or ""
    return out


def to_katakana(text: str) -> str:
    """Shift the hiragana block to katakana (other code points unchanged)."""
    return "".join(chr(ord(ch) + _KATAKANA_SHIFT) if ord(ch) in _HIRAGANA else ch
                   for ch in text)


def _ranked(items: list[tuple[str, bool, bool]]) -> list[str]:
    """Order ``(value, primary, accepted)`` primary first, then accepted first,
    then source order; drop empties and duplicates (first occurrence wins)."""
    indexed = list(enumerate(items))
    indexed.sort(key=lambda t: (not t[1][1], not t[1][2], t[0]))
    out: list[str] = []
    seen: set[str] = set()
    for _, (value, _primary, _accepted) in indexed:
        if value and value not in seen:
            seen.add(value)
            out.append(value)
    return out


def subject_meanings(data: dict, lang: str) -> tuple[list[str], bool]:
    """Primary meanings first, capped to ``MAX_MEANINGS``, plus the fallback flag.

    WaniKani only ships English meanings, so any other language falls back to
    English and reports ``True`` (the card's ``fallback_en`` flag).
    """
    items = [(m["meaning"], bool(m.get("primary")), bool(m.get("accepted_answer")))
             for m in data.get("meanings") or [] if m.get("meaning")]
    return _ranked(items)[:MAX_MEANINGS], lang not in MEANING_LANGS


def subject_readings(data: dict) -> tuple[list[str], list[str]]:
    """``(onyomi, kunyomi)``, primary first, deduped; nanori are ignored.

    On'yomi are shifted to katakana (the API returns hiragana); kun'yomi are
    kept as hiragana.
    """
    on: list[tuple[str, bool, bool]] = []
    kun: list[tuple[str, bool, bool]] = []
    for r in data.get("readings") or []:
        reading = r.get("reading")
        if not reading:
            continue
        primary, accepted = bool(r.get("primary")), bool(r.get("accepted_answer"))
        if r.get("type") == "onyomi":
            on.append((to_katakana(reading), primary, accepted))
        elif r.get("type") == "kunyomi":
            kun.append((reading, primary, accepted))
    return _ranked(on), _ranked(kun)


def default_wanikani_name(levels: Iterable[int] | None = None) -> str:
    picked = sorted(set(levels)) if levels else []
    if not picked:
        return "WaniKani"
    if len(picked) == 1 or picked[-1] == picked[0]:
        return f"WaniKani L{picked[0]}"
    return f"WaniKani L{picked[0]}-{picked[-1]}"


def wanikani_cards(
    token: str,
    *,
    transport: Transport | None = None,
    sleep: Sleep | None = None,
    lang: str = "en",
    levels: Iterable[int] | None = None,
) -> list[Card]:
    """Build cards from the user's **unlocked** WaniKani kanji.

    ``levels`` limits the import to a set of WaniKani levels (``None`` = all
    unlocked). The API filter reduces the payload and the same filter is
    re-applied locally against the subject's level, so a fixture with a locked
    or out-of-range kanji is still excluded without the API's help.
    """
    transport = transport or fetch_json
    sleep = sleep or time.sleep
    wanted = set(levels) if levels is not None else None

    # 1. Assignments: which kanji has this user unlocked and not hidden?
    unlocked: set[int] = set()
    for a in collect_pages(assignments_url(wanted), token, transport, sleep):
        d = a.get("data") or {}
        if d.get("subject_type") != "kanji":
            continue
        if not d.get("unlocked_at") or d.get("hidden"):
            continue
        sid = d.get("subject_id")
        if isinstance(sid, int):
            unlocked.add(sid)

    if not unlocked:
        return []

    # 2. Subjects: the dictionary data, joined by subject id.
    rows: list[tuple[int, int, Card]] = []
    for s in collect_pages(subjects_url(), token, transport, sleep):
        if s.get("object") != "kanji" or s.get("id") not in unlocked:
            continue
        data = s.get("data") or {}
        chars = data.get("characters")
        if not chars or len(chars) != 1:
            print(f"warning: WaniKani subject {s.get('id')} has no single-character "
                  "form; skipped", file=sys.stderr)
            continue
        level = data.get("level") or 0
        if wanted is not None and level not in wanted:
            continue
        meanings, fallback = subject_meanings(data, lang)
        onyomi, kunyomi = subject_readings(data)
        if not meanings and not (onyomi or kunyomi):
            continue
        rows.append((level, s.get("id") or 0,
                     Card(codepoint=ord(chars), meanings=meanings, onyomi=onyomi,
                          kunyomi=kunyomi, fallback_en=fallback)))

    # Ordering hint: WaniKani level first, then subject id (both easiest-first).
    rows.sort(key=lambda r: (r[0], r[1]))
    cards = [c for _, _, c in rows]
    for i, c in enumerate(cards):
        c.level = min(i, 65535)
    return cards
