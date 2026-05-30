"""Tests for is_nonempty_rhistory."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from find_rhistory import is_nonempty_rhistory


def test_empty_string():
    assert not is_nonempty_rhistory("")


def test_whitespace_only():
    assert not is_nonempty_rhistory("   \n\t")


def test_comment_only():
    assert not is_nonempty_rhistory("# comment only\n# another")


def test_real_command():
    assert is_nonempty_rhistory("library(dplyr)")


def test_assignment():
    assert is_nonempty_rhistory("x <- 1\n")


def test_mixed_comments_and_code():
    assert is_nonempty_rhistory("# comment\nlibrary(ggplot2)\n# another comment")


def test_blank_lines_with_code():
    assert is_nonempty_rhistory("\n\n\n  read.csv('data.csv')  \n\n")


def test_only_blank_lines():
    assert not is_nonempty_rhistory("\n\n\n   \n\n")
