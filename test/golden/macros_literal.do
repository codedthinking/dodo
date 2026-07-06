local threshold = 100
global label "north"
scalar factor = 2.5
use "sales.csv"
keep if revenue > `threshold'
display "cutoff is `threshold'"
