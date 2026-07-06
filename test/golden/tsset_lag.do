use "panel.csv"
tsset firm year
generate growth = sales - L.sales
