* The 'in' row-range qualifier is rejected cleanly instead of being
* mistranslated to a column list (fixed: CODE_REVIEW 2.7).
use "sales.csv"
keep in 1/10
