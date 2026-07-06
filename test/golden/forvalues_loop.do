use "sales.csv"
forvalues k = 1/3 {
generate lag`k' = revenue
}
