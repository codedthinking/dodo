#!/usr/bin/env bash
#
# Golden-SQL regression tests for the dodo core.
#
# Each case is a `.do` file plus an expectation:
#   NAME.do + NAME.expected.sql   -> dodoc must exit 0 and emit exactly this SQL
#   NAME.do + NAME.expected.err   -> dodoc must exit non-zero and stderr must
#                                    contain the (trimmed) contents of the .err file
#
# The suite drives the whole core pipeline through the standalone `dodoc`
# compiler, so it needs no DuckDB build and runs in well under a second.
#
# A `.do` whose header comment contains "KNOWN-BAD" documents a currently-wrong
# behavior: its expectation captures today's (buggy) output so the suite stays
# green. When a fix lands, regenerate the expectation (UPDATE=1) and drop the
# KNOWN-BAD marker from the .do file.
#
# Usage:
#   bash test/golden/run_golden.sh          # run all cases
#   UPDATE=1 bash test/golden/run_golden.sh # regenerate every .expected.sql
#   DODOC=/path/to/dodoc bash ...           # use a specific dodoc binary
#
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
DODOC="${DODOC:-$ROOT/build/dodoc/dodoc}"
UPDATE="${UPDATE:-0}"

if [[ ! -x "$DODOC" ]]; then
	echo "dodoc not found at $DODOC — building..." >&2
	make -C "$ROOT" dodoc >/dev/null 2>&1 || { echo "FATAL: could not build dodoc" >&2; exit 2; }
fi

pass=0
fail=0
known=0
regen=0
declare -a failures=()

tmp_err="$(mktemp)"
trap 'rm -f "$tmp_err"' EXIT

for do_file in "$HERE"/*.do; do
	[[ -e "$do_file" ]] || continue
	name="$(basename "$do_file" .do)"
	err_file="$HERE/$name.expected.err"
	sql_file="$HERE/$name.expected.sql"
	is_known=0
	grep -q 'KNOWN-BAD' "$do_file" && is_known=1

	# Run dodoc from the repo root so relative data paths inside the .do resolve.
	actual_out="$(cd "$ROOT" && "$DODOC" "$do_file" 2>"$tmp_err")"
	code=$?
	actual_err="$(cat "$tmp_err")"

	if [[ -f "$err_file" ]]; then
		# Error case: expect failure + substring in stderr.
		want="$(cat "$err_file")"
		if [[ $code -ne 0 && "$actual_err" == *"$want"* ]]; then
			pass=$((pass + 1))
			[[ $is_known -eq 1 ]] && known=$((known + 1))
		else
			fail=$((fail + 1))
			failures+=("$name (expected error containing '$want'; exit=$code)")
		fi
		continue
	fi

	# SQL snapshot case.
	if [[ "$UPDATE" == "1" ]]; then
		printf '%s\n' "$actual_out" > "$sql_file"
		regen=$((regen + 1))
		continue
	fi

	if [[ ! -f "$sql_file" ]]; then
		fail=$((fail + 1))
		failures+=("$name (no expectation; run UPDATE=1 to create $name.expected.sql)")
		continue
	fi

	if [[ "$actual_out" == "$(cat "$sql_file")" ]]; then
		pass=$((pass + 1))
		[[ $is_known -eq 1 ]] && known=$((known + 1))
	else
		fail=$((fail + 1))
		failures+=("$name")
		if [[ "${VERBOSE:-0}" == "1" ]]; then
			echo "--- diff: $name ---"
			diff <(cat "$sql_file") <(printf '%s\n' "$actual_out") || true
		fi
	fi
done

echo
if [[ "$UPDATE" == "1" ]]; then
	echo "golden: regenerated $regen .expected.sql snapshot(s); $pass error-case(s) left untouched."
	exit 0
fi

echo "golden: $pass passed, $fail failed ($known KNOWN-BAD documenting open bugs)."
if [[ $fail -gt 0 ]]; then
	printf '  FAIL: %s\n' "${failures[@]}"
	echo "  (re-run with VERBOSE=1 to see diffs, or UPDATE=1 to accept new output)"
	exit 1
fi
exit 0
