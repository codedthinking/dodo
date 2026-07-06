use "panel.csv"
bysort firm (year): generate cum_sales = sum(sales)
