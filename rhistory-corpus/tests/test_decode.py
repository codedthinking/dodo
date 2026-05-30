"""Tests for base64 decoding behavior."""

import base64
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))


def test_utf8_decode():
    text = "library(dplyr)\nx <- 1\n"
    encoded = base64.b64encode(text.encode("utf-8")).decode("ascii")
    decoded = base64.b64decode(encoded).decode("utf-8", errors="replace")
    assert decoded == text


def test_latin1_with_replacement():
    raw = b"x <- 1\ncomment with \xe9\n"
    encoded = base64.b64encode(raw).decode("ascii")
    decoded = base64.b64decode(encoded).decode("utf-8", errors="replace")
    assert "x <- 1" in decoded
    assert "\ufffd" in decoded  # replacement character


def test_empty_content():
    encoded = base64.b64encode(b"").decode("ascii")
    decoded = base64.b64decode(encoded).decode("utf-8", errors="replace")
    assert decoded == ""


def test_pure_ascii():
    text = "summary(lm(y ~ x))\n"
    encoded = base64.b64encode(text.encode("ascii")).decode("ascii")
    decoded = base64.b64decode(encoded).decode("utf-8", errors="replace")
    assert decoded == text
