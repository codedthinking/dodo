use "sales.csv"
egen region_avg = mean(revenue), by(region)
