* KNOWN-BAD (CODE_REVIEW 2.5): nested multi-line loops lose the inner body.
use "sales.csv"
foreach a in 1 2 {
foreach b in 3 4 {
generate v`a'`b' = `a'*`b'
}
}
