#!/usr/bin/env python3
"""Parse .Rhistory files into per-command JSONL records.

Steps:
  1. Strip comments.
  2. Join continuation lines (unclosed parens/brackets/braces, trailing
     pipes, trailing operators).
  3. Split compound statements (semicolons, braces at statement boundaries).
  4. Extract every function call from each logical line.
  5. Write one JSONL record per source file.

Usage:
  python parse_rhistory.py --raw-dir data/raw_rhistory --out data/parsed.jsonl
"""

from __future__ import annotations

import argparse
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
        description="Parse .Rhistory files into per-command JSONL records."
    )
    parser.add_argument(
        "--raw-dir",
        required=True,
        help="Directory containing raw .Rhistory files",
    )
    parser.add_argument(
        "--out",
        default="parsed.jsonl",
        help="Output JSONL path (default: parsed.jsonl)",
    )
    parser.add_argument(
        "--index",
        default=None,
        help="Optional input index JSONL (to enrich records with metadata)",
    )
    args = parser.parse_args()

    raw_dir = Path(args.raw_dir)
    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)

    # Load index if provided, keyed by raw_file.
    index_map: dict[str, dict[str, Any]] = {}
    if args.index:
        with open(args.index, encoding="utf-8") as f:
            for line in f:
                rec = json.loads(line)
                rf = rec.get("raw_file")
                if rf:
                    index_map[rf] = rec

    files = sorted(raw_dir.glob("*.Rhistory"))
    total_commands = 0
    total_files = 0

    with open(out_path, "w", encoding="utf-8") as fout:
        for fpath in files:
            text = fpath.read_text(encoding="utf-8", errors="replace")
            commands = parse_rhistory(text)
            if not commands:
                continue

            source_id = fpath.name
            meta = index_map.get(source_id, {})

            record = {
                "source_file": source_id,
                "owner": meta.get("owner", ""),
                "repo": meta.get("repo", ""),
                "repo_full_name": meta.get("repo_full_name", ""),
                "command_count": len(commands),
                "commands": commands,
            }
            fout.write(json.dumps(record, ensure_ascii=False) + "\n")
            total_files += 1
            total_commands += len(commands)

            if total_files % 100 == 0:
                print(
                    f"Processed {total_files} files, {total_commands} commands",
                    file=sys.stderr,
                )

    print(
        f"Done. {total_files} files, {total_commands} commands. Output: {out_path}",
        file=sys.stderr,
    )


if __name__ == "__main__":
    main()
