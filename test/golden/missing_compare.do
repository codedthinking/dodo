* Comparisons against Stata's missing sentinel '.' become NULL tests, not
* arithmetic against NULL (fixed: CODE_REVIEW 2.4).
use "sales.csv"
replace revenue = 0 if revenue >= .
keep if price < .
