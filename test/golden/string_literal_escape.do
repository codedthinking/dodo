* Double-quoted strings in expressions convert to SQL literals with embedded
* apostrophes doubled: "it's" -> 'it''s' (was invalid 'it's').
use "sales.csv"
keep if name == "it's"
