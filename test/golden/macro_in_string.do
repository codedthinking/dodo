* Macros expand inside double-quoted strings (Stata behavior), but scalar
* names inside strings are NOT substituted (fixed: CODE_REVIEW 2.2 / 2.3).
global CUR "USD"
scalar region = 5
use "sales.csv"
generate paid = "amount in $CUR"
generate note = "region is here"
