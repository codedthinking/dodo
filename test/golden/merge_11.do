use "sales.csv"
merge 1:1 id using "regions.csv", keep(match master)
