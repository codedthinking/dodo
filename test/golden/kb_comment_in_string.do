* KNOWN-BAD (CODE_REVIEW 2.1): // inside a string literal is treated as a comment,
* corrupting the string and leaving an unbalanced quote.
use "sales.csv"
generate url = "http://example.com"
