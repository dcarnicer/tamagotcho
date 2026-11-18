"""Tests for the fetch retry (stdlib unittest style, run by pytest)."""
from __future__ import annotations

import urllib.request

import pytest

from tamagotcho_pack import fetch


class _Response:
    def __init__(self, data: bytes):
        self.data = data

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False

    def read(self) -> bytes:
        return self.data


def test_download_retries_a_reset(monkeypatch):
    calls = []

    def fake(req, timeout):
        calls.append(timeout)
        if len(calls) < 3:
            raise ConnectionResetError("reset")
        return _Response(b"ok")

    monkeypatch.setattr(urllib.request, "urlopen", fake)
    assert fetch.download("https://example.invalid/x", sleep=lambda _: None) == b"ok"
    assert len(calls) == 3


def test_download_gives_up_after_the_attempts(monkeypatch):
    calls = []

    def fake(req, timeout):
        calls.append(timeout)
        raise ConnectionResetError("reset")

    monkeypatch.setattr(urllib.request, "urlopen", fake)
    with pytest.raises(ConnectionResetError):
        fetch.download("https://example.invalid/x", attempts=3, sleep=lambda _: None)
    assert len(calls) == 3
