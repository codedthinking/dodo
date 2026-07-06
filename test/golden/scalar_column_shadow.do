* Documented deviation (docs/VARIABLE_SUBSTITUTION.md): with no schema at
* compile time, a bare name shared by a scalar and a column resolves to the
* scalar. Use scalar(name) to be explicit; a same-named column is untouched.
scalar factor = 3
use "sales.csv"
generate bare = factor * 2
generate explicit = scalar(factor) * revenue
