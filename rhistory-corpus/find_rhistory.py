#!/usr/bin/env python3
"""Search GitHub for .Rhistory files and collect metadata + optional raw content."""

from __future__ import annotations

import argparse
import base64
import json
import os
import re
import sys
import time
from pathlib import Path
from typing import Any
from urllib.error import HTTPError
from urllib.parse import urlencode
from urllib.request import Request, urlopen


# ---------------------------------------------------------------------------
# GitHub API helpers
# ---------------------------------------------------------------------------

GITHUB_API = "https://api.github.com"
USER_AGENT = "rhistory-research-script"
API_VERSION = "2022-11-28"
REQUEST_TIMEOUT = 30
TRANSIENT_CODES = {502, 503, 504}
MAX_RETRIES = 4


def _headers(token: str | None) -> dict[str, str]:
    h = {
        "Accept": "application/vnd.github+json",
        "X-GitHub-Api-Version": API_VERSION,
        "User-Agent": USER_AGENT,
    }
    if token:
        h["Authorization"] = f"Bearer {token}"
    return h


def _handle_rate_limit(resp_headers: dict[str, str]) -> None:
    remaining = resp_headers.get("X-RateLimit-Remaining")
    if remaining is not None and int(remaining) == 0:
        reset_ts = int(resp_headers.get("X-RateLimit-Reset", "0"))
        wait = max(reset_ts - int(time.time()) + 2, 1)
        print(f"Rate limited. Sleeping {wait}s until reset.", file=sys.stderr)
        time.sleep(wait)


def gh_get(
    url: str,
    token: str | None,
    params: dict[str, Any] | None = None,
) -> dict[str, Any] | list[Any]:
    if params:
        url = f"{url}?{urlencode(params)}"
    req = Request(url, headers=_headers(token))

    for attempt in range(1, MAX_RETRIES + 1):
        try:
            with urlopen(req, timeout=REQUEST_TIMEOUT) as resp:
                headers = {k.lower(): v for k, v in resp.headers.items()}
                _handle_rate_limit(headers)
                return json.loads(resp.read().decode())
        except HTTPError as e:
            headers = {k.lower(): v for k, v in e.headers.items()}
            if e.code == 403 and headers.get("x-ratelimit-remaining") == "0":
                _handle_rate_limit(headers)
                continue
            if e.code in TRANSIENT_CODES and attempt < MAX_RETRIES:
                backoff = 2 ** attempt
                print(
                    f"HTTP {e.code}, retrying in {backoff}s (attempt {attempt}/{MAX_RETRIES})",
                    file=sys.stderr,
                )
                time.sleep(backoff)
                continue
            raise
    raise RuntimeError(f"Failed after {MAX_RETRIES} retries: {url}")


# ---------------------------------------------------------------------------
# Search
# ---------------------------------------------------------------------------


def search_code(
    token: str | None,
    query: str,
    page: int,
    per_page: int,
) -> list[dict[str, Any]]:
    data = gh_get(
        f"{GITHUB_API}/search/code",
        token,
        {"q": query, "per_page": per_page, "page": page},
    )
    return data.get("items", [])


# ---------------------------------------------------------------------------
# Fetch file contents
# ---------------------------------------------------------------------------


def fetch_file_content(
    token: str | None,
    owner: str,
    repo: str,
    path: str,
    ref: str,
) -> tuple[str | None, dict[str, Any]]:
    url = f"{GITHUB_API}/repos/{owner}/{repo}/contents/{path}"
    try:
        data = gh_get(url, token, {"ref": ref})
    except HTTPError as e:
        print(f"  Could not fetch {owner}/{repo}/{path}: HTTP {e.code}", file=sys.stderr)
        return None, {}

    if not isinstance(data, dict):
        return None, {}

    content_b64 = data.get("content")
    if content_b64:
        raw_bytes = base64.b64decode(content_b64)
        text = raw_bytes.decode("utf-8", errors="replace")
        return text, data

    download_url = data.get("download_url")
    if download_url:
        req = Request(download_url, headers=_headers(token))
        try:
            with urlopen(req, timeout=REQUEST_TIMEOUT) as resp:
                raw_bytes = resp.read()
                text = raw_bytes.decode("utf-8", errors="replace")
                return text, data
        except HTTPError:
            pass

    return None, data


# ---------------------------------------------------------------------------
# Nonempty filter
# ---------------------------------------------------------------------------


def is_nonempty_rhistory(text: str) -> bool:
    for line in text.splitlines():
        stripped = line.strip()
        if stripped and not stripped.startswith("#"):
            return True
    return False


# ---------------------------------------------------------------------------
# Safe filename
# ---------------------------------------------------------------------------

_UNSAFE_RE = re.compile(r'[^a-zA-Z0-9._-]')


def safe_filename(owner: str, repo: str, path: str, sha: str) -> str:
    sanitized_path = _UNSAFE_RE.sub("_", path)
    sha12 = sha[:12]
    return f"{owner}__{repo}__{sanitized_path}__{sha12}.Rhistory"


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Search GitHub for .Rhistory files and collect metadata."
    )
    parser.add_argument(
        "--out",
        default="rhistory.jsonl",
        help="Output JSONL index path (default: rhistory.jsonl)",
    )
    parser.add_argument(
        "--save-raw",
        default=None,
        metavar="DIR",
        help="Directory for raw .Rhistory files (optional)",
    )
    parser.add_argument(
        "--max-pages",
        type=int,
        default=10,
        help="Max GitHub search result pages (default: 10)",
    )
    parser.add_argument(
        "--per-page",
        type=int,
        default=100,
        help="Results per page (default: 100)",
    )
    parser.add_argument(
        "--query-extra",
        default="",
        help="Extra GitHub search qualifiers",
    )
    parser.add_argument(
        "--token",
        default=None,
        help="GitHub token (default: GITHUB_TOKEN env var)",
    )
    args = parser.parse_args()

    token = args.token or os.environ.get("GITHUB_TOKEN")
    if not token:
        print(
            "Warning: No GitHub token. Unauthenticated requests have very low rate limits.",
            file=sys.stderr,
        )

    print(
        "Warning: .Rhistory files may contain private paths, credentials, names, "
        "project identifiers, database URLs, and other sensitive data. "
        "Treat raw files as sensitive. Do not publish raw files.",
        file=sys.stderr,
    )

    save_raw_dir: Path | None = None
    if args.save_raw:
        save_raw_dir = Path(args.save_raw)
        save_raw_dir.mkdir(parents=True, exist_ok=True)

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)

    query = f"filename:.Rhistory {args.query_extra}".strip()

    seen: set[tuple[str, str, str, str]] = set()
    checked = 0
    kept = 0

    with open(out_path, "w", encoding="utf-8") as fout:
        for page in range(1, args.max_pages + 1):
            print(f"Searching page {page}: {query}", file=sys.stderr)
            items = search_code(token, query, page, args.per_page)

            if not items:
                print("No more results.", file=sys.stderr)
                break

            for item in items:
                owner = item["repository"]["owner"]["login"]
                repo_name = item["repository"]["name"]
                path = item["path"]
                sha = item["sha"]

                key = (owner, repo_name, path, sha)
                if key in seen:
                    continue
                seen.add(key)

                checked += 1

                text, content_obj = fetch_file_content(token, owner, repo_name, path, sha)

                if text is None:
                    continue

                if not is_nonempty_rhistory(text):
                    continue

                raw_file: str | None = None
                if save_raw_dir:
                    raw_file = safe_filename(owner, repo_name, path, sha)
                    (save_raw_dir / raw_file).write_text(text, encoding="utf-8")

                lines = text.splitlines()
                record = {
                    "owner": owner,
                    "repo": repo_name,
                    "repo_full_name": item["repository"]["full_name"],
                    "repo_html_url": item["repository"]["html_url"],
                    "path": path,
                    "sha": sha,
                    "html_url": item["html_url"],
                    "git_url": content_obj.get("git_url", ""),
                    "size_bytes": len(text.encode("utf-8")),
                    "line_count": len(lines),
                    "nonblank_line_count": sum(1 for l in lines if l.strip()),
                    "raw_file": raw_file,
                }
                fout.write(json.dumps(record, ensure_ascii=False) + "\n")
                kept += 1

            print(f"Checked={checked}, kept={kept}", file=sys.stderr)

    print(f"Done. Checked {checked} files, kept {kept}. Output: {out_path}", file=sys.stderr)


if __name__ == "__main__":
    main()
