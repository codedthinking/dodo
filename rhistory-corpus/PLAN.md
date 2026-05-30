# PLAN.md: GitHub `.Rhistory` Corpus Builder

## Goal

Build a Python tool that searches GitHub for accidentally committed, nonempty `.Rhistory` files, downloads metadata and optionally raw contents, and prepares the data for later sanitization and workflow analysis.

The tool should be suitable for research on real interactive data-cleaning and statistical-analysis workflows.

## Core requirements

### 1. Search GitHub for `.Rhistory` files

Use the GitHub REST API code search endpoint.

Search query:

```text
filename:.Rhistory
```

Support optional extra qualifiers from the command line, for example:

```text
language:R
stars:>10
read.csv OR library OR ggplot
```

The tool should paginate results.

Parameters:

* `--max-pages`
* `--per-page`
* `--query-extra`
* `--token`, defaulting to `GITHUB_TOKEN`

Expected command:

```bash
python find_rhistory.py \
  --out data/rhistory_index.jsonl \
  --save-raw data/raw_rhistory \
  --max-pages 10
```

### 2. Fetch file contents

For every search result:

1. Extract:

   * owner
   * repo
   * repo full name
   * path
   * file SHA
   * HTML URL
   * repository URL

2. Fetch file contents through the GitHub repository contents API.

3. Decode base64 content.

4. If the file cannot be decoded as UTF-8, decode with replacement characters rather than crashing.

### 3. Keep only nonempty files

A file is nonempty if, after stripping whitespace, it contains at least one meaningful line.

Ignore files that are:

* empty
* whitespace only
* comment-only, if the comment-only state is easy to detect

Basic rule:

```python
meaningful_lines = [
    line for line in text.splitlines()
    if line.strip() and not line.strip().startswith("#")
]
is_nonempty = len(meaningful_lines) > 0
```

### 4. Write JSONL metadata index

Write one JSON object per kept `.Rhistory` file.

Required fields:

```json
{
  "owner": "...",
  "repo": "...",
  "repo_full_name": "...",
  "repo_html_url": "...",
  "path": "...",
  "sha": "...",
  "html_url": "...",
  "git_url": "...",
  "size_bytes": 1234,
  "line_count": 100,
  "nonblank_line_count": 80,
  "raw_file": "optional_local_filename.Rhistory"
}
```

Use JSON Lines format:

```text
one object per line
UTF-8
no trailing commas
```

### 5. Optionally save raw files

If `--save-raw DIR` is provided, save the raw `.Rhistory` text locally.

Filename should be deterministic and collision-resistant.

Suggested format:

```text
{owner}__{repo}__{sanitized_path}__{sha12}.Rhistory
```

Sanitize path characters so the filename is safe on Unix and Windows.

If `--save-raw` is omitted, do not save raw text, only metadata.

### 6. Handle duplicates

Avoid duplicate processing within one run.

Deduplicate by:

```python
(owner, repo, path, sha)
```

### 7. Handle rate limits and transient errors

Implement robust HTTP handling:

* Use authenticated requests if `GITHUB_TOKEN` exists.
* If rate-limited, read `X-RateLimit-Reset`, sleep until reset plus a small buffer, then retry.
* Retry transient errors:

  * 502
  * 503
  * 504
* Use a timeout on all requests.
* Fail clearly on persistent errors.

### 8. Be careful with privacy

`.Rhistory` files may contain sensitive information.

The initial tool may download raw files, but it must print a warning such as:

```text
Warning: .Rhistory files may contain private paths, credentials, names, project identifiers, database URLs, and other sensitive data. Treat raw files as sensitive. Do not publish raw files.
```

Do not upload raw files anywhere.

Do not print raw command contents to stdout.

## CLI design

Use `argparse`.

Required or defaulted arguments:

```text
--out PATH
    Output JSONL index.
    Default: rhistory.jsonl

--save-raw PATH
    Optional directory for raw .Rhistory files.

--max-pages INT
    Max GitHub search result pages.
    Default: 10

--per-page INT
    Results per page.
    Default: 100

--query-extra TEXT
    Extra GitHub search qualifiers.
    Default: ""

--token TEXT
    GitHub token.
    Default: environment variable GITHUB_TOKEN
```

## Example command

```bash
export GITHUB_TOKEN=ghp_xxx

python find_rhistory.py \
  --out data/rhistory_index.jsonl \
  --save-raw data/raw_rhistory \
  --max-pages 20 \
  --per-page 100
```

## Ethical handling

Treat raw `.Rhistory` files as sensitive research data.

Do not commit:

```text
data/raw_rhistory/
data/*.jsonl
.env
```

Only publish:

* aggregate statistics
* sanitized command sequences
* derived features
* code for collection and sanitization

Do not publish raw histories.
