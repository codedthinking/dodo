* KNOWN-BAD (CODE_REVIEW 2.2): $ident is macro-expanded inside a string literal
* and silently deleted.
use "sales.csv"
generate note = "paid in $USD today"
