* KNOWN-BAD (CODE_REVIEW 2.9): 'undo abc' throws a raw std::invalid_argument,
* which currently ABORTS dodoc (exit 134) instead of a clean error.
use "sales.csv"
generate x = 1
undo abc
