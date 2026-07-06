* KNOWN-BAD (CODE_REVIEW 2.4): x >= . should mean "x is missing" (IS NULL),
* but compiles to x >= NULL which is never true.
use "sales.csv"
replace revenue = 0 if revenue >= .
