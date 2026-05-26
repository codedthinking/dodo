* Load the firms data
use "test/data/firms.csv", clear

// Keep only 2018 data
keep if year == 2018

/* Generate a new variable
   with the log of revenue */
generate ln_rev = log(revenue)

// Sort by revenue
sort revenue
