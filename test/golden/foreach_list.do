use "sales.csv"
foreach v in revenue cost {
generate log_`v' = log(`v')
}
