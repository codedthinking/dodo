#!/usr/bin/env python3
"""Parse .Rhistory files into normalized CSV tables.

Steps:
  1. Strip comments.
  2. Join continuation lines (unclosed parens/brackets/braces, trailing
     pipes, trailing operators).
  3. Split compound statements (semicolons at statement boundaries).
  4. Extract every function call from each logical line.
  5. Write three CSV files:
       files.csv          - one row per .Rhistory file
       commands.csv       - one row per command invocation (FK: file_id)
       function_calls.csv - one row per function call    (FK: file_id, command_index)

Usage:
  python parse_rhistory.py --raw-dir data/raw_rhistory --out-dir data
"""

from __future__ import annotations

import argparse
import csv
import json
import re
import sys
from pathlib import Path
from typing import Any


# ---------------------------------------------------------------------------
# 1. Strip comments
# ---------------------------------------------------------------------------

def strip_comments(lines: list[str]) -> list[str]:
    """Remove full-line and inline comments, respecting quoted strings."""
    out: list[str] = []
    for line in lines:
        cleaned = _remove_inline_comment(line)
        if cleaned.strip():
            out.append(cleaned)
    return out


def _remove_inline_comment(line: str) -> str:
    """Remove the first unquoted # and everything after it."""
    in_single = False
    in_double = False
    i = 0
    while i < len(line):
        ch = line[i]
        if ch == "\\" and i + 1 < len(line) and (in_single or in_double):
            i += 2
            continue
        if ch == "'" and not in_double:
            in_single = not in_single
        elif ch == '"' and not in_single:
            in_double = not in_double
        elif ch == "#" and not in_single and not in_double:
            return line[:i]
        i += 1
    return line


# ---------------------------------------------------------------------------
# 2. Join continuation lines
# ---------------------------------------------------------------------------

# Trailing binary operators / pipes that signal continuation.
_TRAILING_OP = re.compile(
    r"(?:"
    r"%>%"          # magrittr pipe
    r"|%<>%"        # magrittr assignment pipe
    r"|%T>%"        # magrittr tee pipe
    r"|\|>"         # base R pipe (4.1+)
    r"|\+"          # ggplot2 layer add
    r"|~"           # formula / lambda
    r"|,"           # trailing comma
    r"|-(?!>)"      # minus (but not ->)
    r"|\*"          # multiply
    r"|/"           # divide
    r"|&&"          # logical and
    r"|\|\|"        # logical or
    r"|&(?!&)"      # bitwise and
    r"|\|(?!>)"     # bitwise or (but not |>)
    r"|<-"          # left assignment
    r"|->(?!>)"     # right assignment
    r"|<<-"         # deep assignment
    r"|={1,2}"      # assignment / equality
    r"|!="          # not equal
    r"|[<>]=?"      # comparison
    r")"
    r"\s*$"
)

# Leading pipe on the next line also signals continuation of the previous.
_LEADING_PIPE = re.compile(r"^\s*(?:%>%|%<>%|%T>%|\|>)")


def _bracket_depth_delta(line: str) -> int:
    """Net change in bracket depth, ignoring brackets inside strings."""
    depth = 0
    in_single = False
    in_double = False
    i = 0
    while i < len(line):
        ch = line[i]
        if ch == "\\" and i + 1 < len(line) and (in_single or in_double):
            i += 2
            continue
        if ch == "'" and not in_double:
            in_single = not in_single
        elif ch == '"' and not in_single:
            in_double = not in_double
        elif not in_single and not in_double:
            if ch in "([{":
                depth += 1
            elif ch in ")]}":
                depth -= 1
        i += 1
    return depth


def join_continuations(lines: list[str]) -> list[str]:
    """Merge lines that are continuations into single logical lines."""
    if not lines:
        return []

    logical: list[str] = []
    buf = lines[0]
    depth = _bracket_depth_delta(buf)

    for line in lines[1:]:
        is_continuation = (
            depth > 0
            or _TRAILING_OP.search(buf)
            or _LEADING_PIPE.match(line)
        )
        if is_continuation:
            buf = buf.rstrip() + " " + line.strip()
            depth += _bracket_depth_delta(line)
            if depth < 0:
                depth = 0
        else:
            logical.append(buf)
            buf = line
            depth = _bracket_depth_delta(buf)

    logical.append(buf)
    return logical


# ---------------------------------------------------------------------------
# 3. Split compound statements
# ---------------------------------------------------------------------------

def split_statements(line: str) -> list[str]:
    """Split on unquoted, unbracketted semicolons."""
    parts: list[str] = []
    current: list[str] = []
    depth = 0
    in_single = False
    in_double = False
    i = 0
    while i < len(line):
        ch = line[i]
        if ch == "\\" and i + 1 < len(line) and (in_single or in_double):
            current.append(ch)
            current.append(line[i + 1])
            i += 2
            continue
        if ch == "'" and not in_double:
            in_single = not in_single
        elif ch == '"' and not in_single:
            in_double = not in_double
        elif not in_single and not in_double:
            if ch in "([{":
                depth += 1
            elif ch in ")]}":
                depth = max(depth - 1, 0)
            elif ch == ";" and depth == 0:
                stmt = "".join(current).strip()
                if stmt:
                    parts.append(stmt)
                current = []
                i += 1
                continue
        current.append(ch)
        i += 1

    stmt = "".join(current).strip()
    if stmt:
        parts.append(stmt)
    return parts


# ---------------------------------------------------------------------------
# 4. Extract function calls
# ---------------------------------------------------------------------------

# Matches R function calls including namespaced ones (pkg::fn, pkg:::fn).
# Captures the full qualified name.  Does NOT match control-flow keywords
# used without function-call syntax (if, else, for, while, repeat).
_FUNC_CALL = re.compile(
    r"(?<![.\w])"                          # not preceded by word/dot char
    r"((?:[a-zA-Z][a-zA-Z0-9_.]*:::{0,1})?"  # optional namespace::
    r"(?:\.?[a-zA-Z][a-zA-Z0-9_.]*)"      # function name (may start with .)
    r")"
    r"\s*\("                               # opening paren
)

_CONTROL_FLOW = {
    "if", "else", "for", "while", "repeat", "return", "next", "break",
    "in", "function", "switch",
}

# Operators that aren't really function calls.
_OPERATOR_LIKE = {"c", "C"}  # keep c() — it's a real function


def extract_functions(statement: str) -> list[str]:
    """Return an ordered list of function names called in *statement*."""
    fns: list[str] = []
    for m in _FUNC_CALL.finditer(statement):
        name = m.group(1)
        # Strip namespace prefix to get the bare name for filtering.
        bare = name.split(":")[-1] if "::" in name else name
        if bare in _CONTROL_FLOW:
            continue
        fns.append(name)
    return fns


# ---------------------------------------------------------------------------
# 5. Detect pipes
# ---------------------------------------------------------------------------

_PIPE_RE = re.compile(r"%>%|%<>%|%T>%|\|>")


def has_pipe(statement: str) -> bool:
    return bool(_PIPE_RE.search(statement))


def pipe_type(statement: str) -> str | None:
    m = _PIPE_RE.search(statement)
    return m.group(0) if m else None


# ---------------------------------------------------------------------------
# 6. Detect assignment
# ---------------------------------------------------------------------------

_ASSIGN_RE = re.compile(
    r"^\s*"
    r"(?:\.?[a-zA-Z][a-zA-Z0-9_.]*)"   # target variable
    r"\s*"
    r"(?:<-|<<-|=(?!=))"               # assignment operator
)


def has_assignment(statement: str) -> bool:
    return bool(_ASSIGN_RE.match(statement))


def assignment_target(statement: str) -> str | None:
    m = _ASSIGN_RE.match(statement)
    if m:
        return m.group(0).split("<-")[0].split("<<-")[0].split("=")[0].strip()
    return None


# ---------------------------------------------------------------------------
# Process one file
# ---------------------------------------------------------------------------


def parse_rhistory(text: str) -> list[dict[str, Any]]:
    """Parse raw .Rhistory text into a list of command records."""
    raw_lines = text.splitlines()
    cleaned = strip_comments(raw_lines)
    logical = join_continuations(cleaned)

    records: list[dict[str, Any]] = []
    cmd_index = 0
    for logical_line in logical:
        for stmt in split_statements(logical_line):
            stmt = stmt.strip()
            if not stmt:
                continue
            fns = extract_functions(stmt)
            rec: dict[str, Any] = {
                "command_index": cmd_index,
                "functions": fns,
                "has_pipe": has_pipe(stmt),
                "has_assignment": has_assignment(stmt),
            }
            pt = pipe_type(stmt)
            if pt:
                rec["pipe_type"] = pt
            tgt = assignment_target(stmt)
            if tgt:
                rec["assignment_target"] = tgt
            if fns:
                rec["primary_function"] = fns[0]
            records.append(rec)
            cmd_index += 1

    return records


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Parse .Rhistory files into normalized CSV tables."
    )
    parser.add_argument(
        "--raw-dir",
        required=True,
        help="Directory containing raw .Rhistory files",
    )
    parser.add_argument(
        "--out-dir",
        default=".",
        help="Output directory for CSV files (default: current directory)",
    )
    parser.add_argument(
        "--index",
        default=None,
        help="Optional input index JSONL (to enrich records with metadata)",
    )
    args = parser.parse_args()

    raw_dir = Path(args.raw_dir)
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    # Load index if provided, keyed by raw_file.
    index_map: dict[str, dict[str, Any]] = {}
    if args.index:
        with open(args.index, encoding="utf-8") as f:
            for line in f:
                rec = json.loads(line)
                rf = rec.get("raw_file")
                if rf:
                    index_map[rf] = rec

    files_path = out_dir / "files.csv"
    commands_path = out_dir / "commands.csv"
    calls_path = out_dir / "function_calls.csv"

    files_sorted = sorted(raw_dir.glob("*.Rhistory"))
    total_commands = 0
    total_calls = 0
    total_files = 0

    with (
        open(files_path, "w", newline="", encoding="utf-8") as f_files,
        open(commands_path, "w", newline="", encoding="utf-8") as f_cmds,
        open(calls_path, "w", newline="", encoding="utf-8") as f_calls,
    ):
        w_files = csv.writer(f_files)
        w_cmds = csv.writer(f_cmds)
        w_calls = csv.writer(f_calls)

        w_files.writerow([
            "file_id", "source_file", "owner", "repo", "repo_full_name",
        ])
        w_cmds.writerow([
            "file_id", "command_index", "primary_function",
            "has_pipe", "pipe_type", "has_assignment", "assignment_target",
        ])
        w_calls.writerow([
            "file_id", "command_index", "call_order", "function_name",
        ])

        file_id = 0
        for fpath in files_sorted:
            text = fpath.read_text(encoding="utf-8", errors="replace")
            commands = parse_rhistory(text)
            if not commands:
                continue

            source_id = fpath.name
            meta = index_map.get(source_id, {})

            w_files.writerow([
                file_id,
                source_id,
                meta.get("owner", ""),
                meta.get("repo", ""),
                meta.get("repo_full_name", ""),
            ])

            for cmd in commands:
                cmd_idx = cmd["command_index"]
                w_cmds.writerow([
                    file_id,
                    cmd_idx,
                    cmd.get("primary_function", ""),
                    cmd["has_pipe"],
                    cmd.get("pipe_type", ""),
                    cmd["has_assignment"],
                    cmd.get("assignment_target", ""),
                ])
                for call_order, fn_name in enumerate(cmd["functions"]):
                    w_calls.writerow([file_id, cmd_idx, call_order, fn_name])
                    total_calls += 1

                total_commands += 1

            total_files += 1
            file_id += 1

            if total_files % 100 == 0:
                print(
                    f"Processed {total_files} files, {total_commands} commands, "
                    f"{total_calls} function calls",
                    file=sys.stderr,
                )

    print(
        f"Done. {total_files} files, {total_commands} commands, "
        f"{total_calls} function calls.",
        file=sys.stderr,
    )
    print(f"  {files_path}", file=sys.stderr)
    print(f"  {commands_path}", file=sys.stderr)
    print(f"  {calls_path}", file=sys.stderr)


if __name__ == "__main__":
    main()
