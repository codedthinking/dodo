* An empty right-hand side clears the macro instead of emitting invalid
* 'SET VARIABLE ... = ' or dereferencing an empty string (fixed: CODE_REVIEW 2.8).
local x =
display "[`x']"
