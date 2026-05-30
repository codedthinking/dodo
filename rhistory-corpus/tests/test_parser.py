"""Tests for parse_rhistory.py."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from parse_rhistory import (
    extract_functions,
    has_pipe,
    join_continuations,
    parse_rhistory,
    split_statements,
    strip_comments,
    assignment_target,
)


# -- strip_comments --

def test_full_line_comment():
    assert strip_comments(["# this is a comment"]) == []


def test_inline_comment():
    assert strip_comments(["x <- 1 # assign"]) == ["x <- 1 "]


def test_hash_in_string():
    result = strip_comments(['x <- "foo#bar"'])
    assert result == ['x <- "foo#bar"']


def test_hash_in_single_quote_string():
    result = strip_comments(["x <- 'foo#bar'"])
    assert result == ["x <- 'foo#bar'"]


# -- join_continuations --

def test_join_unclosed_paren():
    lines = ["f(a,", "  b,", "  c)"]
    result = join_continuations(lines)
    assert len(result) == 1
    assert "f(a," in result[0] and "c)" in result[0]


def test_join_trailing_pipe():
    lines = ["df %>%", "  filter(x > 1) %>%", "  select(y)"]
    result = join_continuations(lines)
    assert len(result) == 1
    assert "%>%" in result[0]


def test_join_base_pipe():
    lines = ["df |>", "  filter(x > 1) |>", "  select(y)"]
    result = join_continuations(lines)
    assert len(result) == 1


def test_join_leading_pipe():
    lines = ["df", "  %>% filter(x > 1)", "  %>% select(y)"]
    result = join_continuations(lines)
    assert len(result) == 1


def test_join_ggplot_plus():
    lines = ["ggplot(df, aes(x, y)) +", "  geom_point() +", "  theme_minimal()"]
    result = join_continuations(lines)
    assert len(result) == 1


def test_no_join_independent_lines():
    lines = ["x <- 1", "y <- 2", "z <- 3"]
    result = join_continuations(lines)
    assert len(result) == 3


def test_join_trailing_comma():
    lines = ["data.frame(a = 1,", "b = 2)"]
    result = join_continuations(lines)
    assert len(result) == 1


def test_join_trailing_assignment():
    lines = ["result <-", "  compute_value()"]
    result = join_continuations(lines)
    assert len(result) == 1


# -- split_statements --

def test_split_semicolons():
    result = split_statements("x <- 1; y <- 2; z <- 3")
    assert len(result) == 3


def test_no_split_in_string():
    result = split_statements('x <- "a;b;c"')
    assert len(result) == 1


def test_no_split_in_parens():
    result = split_statements("f(a; b)")
    assert len(result) == 1


# -- extract_functions --

def test_simple_call():
    assert extract_functions("library(dplyr)") == ["library"]


def test_nested_calls():
    assert extract_functions("mean(log(x))") == ["mean", "log"]


def test_namespaced_call():
    fns = extract_functions("dplyr::mutate(df, y = x + 1)")
    assert "dplyr::mutate" in fns


def test_triple_colon():
    fns = extract_functions("pkg:::internal_fn(x)")
    assert "pkg:::internal_fn" in fns


def test_dotted_function():
    fns = extract_functions("read.csv('data.csv')")
    assert "read.csv" in fns


def test_no_function():
    assert extract_functions("x <- 1 + 2") == []


def test_control_flow_excluded():
    fns = extract_functions("if (x > 0) print(x)")
    assert "if" not in fns
    assert "print" in fns


def test_for_excluded():
    fns = extract_functions("for (i in 1:10) print(i)")
    assert "for" not in fns
    assert "print" in fns


def test_function_definition_excluded():
    fns = extract_functions("f = function(x) x + 1")
    assert "function" not in fns


def test_c_call():
    fns = extract_functions("c(1, 2, 3)")
    assert "c" in fns


# -- pipes --

def test_pipe_magrittr():
    assert has_pipe("df %>% filter(x > 1)")


def test_pipe_base():
    assert has_pipe("df |> filter(x > 1)")


def test_no_pipe():
    assert not has_pipe("filter(df, x > 1)")


# -- assignment --

def test_assignment_arrow():
    assert assignment_target("result <- f(x)") == "result"


def test_assignment_equals():
    assert assignment_target("result = f(x)") == "result"


def test_no_assignment():
    assert assignment_target("f(x)") is None


def test_double_equals_not_assignment():
    assert assignment_target("x == 1") is None


# -- full parse --

def test_full_parse_pipe_chain():
    text = "df %>%\n  filter(x > 1) %>%\n  select(y)"
    records = parse_rhistory(text)
    assert len(records) == 1
    assert records[0]["has_pipe"]
    assert "filter" in records[0]["functions"]
    assert "select" in records[0]["functions"]


def test_full_parse_simple():
    text = "library(dplyr)\nx <- read.csv('data.csv')\nsummary(x)"
    records = parse_rhistory(text)
    assert len(records) == 3
    assert records[0]["primary_function"] == "library"
    assert records[1]["primary_function"] == "read.csv"
    assert records[1]["has_assignment"]
    assert records[2]["primary_function"] == "summary"


def test_full_parse_comments_stripped():
    text = "# load data\nlibrary(dplyr)\n# done"
    records = parse_rhistory(text)
    assert len(records) == 1
    assert records[0]["primary_function"] == "library"


def test_full_parse_multiline_call():
    text = "lm(y ~ x,\n  data = df,\n  weights = w)"
    records = parse_rhistory(text)
    assert len(records) == 1
    assert records[0]["primary_function"] == "lm"
