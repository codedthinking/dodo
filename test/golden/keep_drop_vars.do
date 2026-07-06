* Column selection and row filters.
use "sales.csv"
keep id region revenue
drop if revenue < 0
