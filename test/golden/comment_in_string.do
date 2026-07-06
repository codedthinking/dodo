* // inside a string literal (e.g. a URL) is not treated as a comment; Stata
* only starts a // comment after whitespace (fixed: CODE_REVIEW 2.1).
use "sales.csv"
generate url = "http://example.com"
generate note = "value" // this trailing comment is stripped
