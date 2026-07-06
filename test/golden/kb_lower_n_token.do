* KNOWN-BAD (CODE_REVIEW 2.6): _n (row index) is conflated with _N (row count)
* and emits invalid SET VARIABLE SQL.
use "sales.csv"
local i = _n + 1
