* Comparisons against Stata's missing sentinel '.' become NULL tests, not
* arithmetic against NULL (CODE_REVIEW 2.4). Covers identifier, parenthesized,
* and function-call operands, and missing-on-the-left.
use "sales.csv"
replace revenue = 0 if revenue >= .
keep if price < .
keep if (revenue + cost) >= .
keep if substr(region,1,1) != .
keep if . > price
