* KNOWN-BAD (CODE_REVIEW 2.3): a scalar silently shadows a same-named column;
* revenue should read the column, not the scalar 5.
scalar revenue = 5
use "sales.csv"
generate doubled = revenue * 2
