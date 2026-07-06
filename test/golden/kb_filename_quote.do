* KNOWN-BAD (CODE_REVIEW 2.11): apostrophe in a filename is pasted unescaped
* into the SQL string literal, breaking the query.
use "it's data.csv"
