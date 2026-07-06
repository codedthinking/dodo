* A non-numeric forvalues range raises a clean error instead of aborting
* the process (fixed: CODE_REVIEW 2.9).
forvalues i = a/b {
display 1
}
