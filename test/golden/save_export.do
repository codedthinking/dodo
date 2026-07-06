use "sales.csv"
keep if revenue > 0
save "clean.parquet"
export delimited using "clean.csv"
