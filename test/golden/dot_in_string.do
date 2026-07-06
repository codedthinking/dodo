* A bare '.' inside a string literal is NOT rewritten to NULL (the missing-value
* passes are quote-aware).
use "sales.csv"
generate s = "a . b"
