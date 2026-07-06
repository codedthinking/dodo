* Core happy path: load, filter, derive, inspect.
use "sales.csv"
keep if region == "north"
generate margin = revenue - cost
count
