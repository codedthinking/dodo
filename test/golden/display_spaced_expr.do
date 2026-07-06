* display accumulates a spaced expression as one token instead of splitting
* on whitespace into CAST(+ AS VARCHAR) (fixed: CODE_REVIEW 2.10).
local x = 1
display `x' + 1
