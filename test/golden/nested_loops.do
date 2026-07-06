* Nested multi-line loops expand correctly; the inner body is no longer
* lost (fixed: CODE_REVIEW 2.5).
use "sales.csv"
foreach a in 1 2 {
foreach b in 3 4 {
generate v`a'`b' = `a'*`b'
}
}
