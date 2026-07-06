* _n (observation index) is distinct from _N (count) and cannot be resolved
* in a macro assignment — clean error, not invalid SQL (fixed: CODE_REVIEW 2.6).
use "sales.csv"
local i = _n + 1
