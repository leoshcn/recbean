"""Hotword budget: token estimate and selection of the words actually sent to the ASR.

The Doubao bidirectional streaming API accepts hotwords inline through
``request.corpus.context`` (a JSON *string* ``{"hotwords":[{"word":..}]}``) and documents a
limit of roughly 100 tokens without saying how tokens are counted or what happens when the
limit is exceeded. Measured against the real API (2026-10-18, see
.trellis/spec/backend/doubao-asr-and-deploy.md): up to 10 001 words neither errored nor
were visibly truncated, so the limit is not enforced. We still keep our own conservative
estimate and only send the words that fit ``HOTWORD_TOKEN_BUDGET``, in list order (the
order the user sees is the priority).

Estimate per word: one token per CJK character, one per run of ASCII letters/digits, one per
other non-space symbol, plus one separator token per word.
"""

from __future__ import annotations

import json
import re
import unicodedata
from typing import Iterable, Optional

WORD_MAX_CHARS = 32
SEPARATOR_TOKENS = 1

_ALNUM_RUN = re.compile(r"[A-Za-z0-9]+")


def normalize_word(raw: str) -> str:
    return unicodedata.normalize("NFC", str(raw)).strip()


def validate_word(word: str) -> Optional[str]:
    """Return an error message for an invalid (already normalized) word, else None."""
    if not word:
        return "热词不能为空"
    if len(word) > WORD_MAX_CHARS:
        return f"热词最多 {WORD_MAX_CHARS} 个字"
    if "\n" in word or "\r" in word:
        return "热词不能包含换行"
    return None


def estimate_tokens(word: str) -> int:
    n = 0
    rest = _ALNUM_RUN.sub(lambda m: "\x00", word)
    for ch in rest:
        if ch == "\x00":
            n += 1  # one ASCII letter/digit run
        elif ch.isspace():
            continue
        else:
            n += 1  # CJK character or other symbol
    return n + SEPARATOR_TOKENS


def select_active(words: Iterable[str], budget: int) -> tuple[list[bool], int]:
    """Mark words active from the top while the running total fits ``budget``.

    The first word that does not fit and everything after it are inactive (we do not skip
    ahead to squeeze in shorter words). Returns (flags, used_tokens).
    """
    flags: list[bool] = []
    used = 0
    full = False
    for w in words:
        t = estimate_tokens(w)
        if not full and used + t <= budget:
            used += t
            flags.append(True)
        else:
            full = True
            flags.append(False)
    return flags, used


def active_words(words: Iterable[str], budget: int) -> list[str]:
    words = list(words)
    flags, _ = select_active(words, budget)
    return [w for w, ok in zip(words, flags) if ok]


def corpus_context(words: list[str]) -> str:
    """The string sent as ``request.corpus.context``."""
    return json.dumps({"hotwords": [{"word": w} for w in words]}, ensure_ascii=False)
