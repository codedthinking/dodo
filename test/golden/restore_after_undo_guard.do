* undo may not cross an active preserve checkpoint (would corrupt the chain).
use "sales.csv"
generate a = 1
preserve
generate b = 2
undo 2
