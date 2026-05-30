"""Tests for safe_filename."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from find_rhistory import safe_filename


def test_basic():
    result = safe_filename("alice", "myrepo", ".Rhistory", "abcdef123456789")
    assert result == "alice__myrepo__.Rhistory__abcdef123456.Rhistory"


def test_nested_path():
    result = safe_filename("bob", "proj", "src/analysis/.Rhistory", "0123456789abcdef")
    assert result == "bob__proj__src_analysis_.Rhistory__0123456789ab.Rhistory"


def test_unsafe_chars():
    result = safe_filename("user", "repo", "path with spaces/.Rhistory", "aabbccddee11")
    assert " " not in result
    assert result.endswith(".Rhistory")


def test_sha_truncation():
    result = safe_filename("u", "r", "p", "a" * 40)
    assert "__aaaaaaaaaaaa.Rhistory" in result


def test_deterministic():
    a = safe_filename("x", "y", "z", "sha123")
    b = safe_filename("x", "y", "z", "sha123")
    assert a == b
