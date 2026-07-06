* KNOWN-BAD (CODE_REVIEW 2.10): display splits a spaced expression on whitespace,
* producing CAST(+ AS VARCHAR).
local x = 1
display `x' + 1
