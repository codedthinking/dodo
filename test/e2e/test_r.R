# E2E test: R DBI client with dodo extension
library(DBI)
library(duckdb)

args <- commandArgs(trailingOnly = TRUE)
project_dir <- if (length(args) > 0) args[1] else getwd()
ext_path <- file.path(project_dir, "build", "release", "extension", "dodo", "dodo.duckdb_extension")

if (!file.exists(ext_path)) {
    cat("FAIL: Extension not found at", ext_path, "\n")
    quit(status = 1)
}

failures <- 0L

run_test <- function(name, expr) {
    tryCatch({
        eval(expr)
        cat("  PASS:", name, "\n")
    }, error = function(e) {
        cat("  FAIL:", name, "\n")
        cat("   ", conditionMessage(e), "\n")
        failures <<- failures + 1L
    })
}

fresh_conn <- function() {
    con <- dbConnect(duckdb::duckdb(), dbdir = ":memory:")
    dbExecute(con, "SET allow_unsigned_extensions = true")
    dbExecute(con, sprintf("LOAD '%s'", ext_path))
    con
}

data_path <- file.path(project_dir, "test", "data", "firms.csv")

cat("=== R e2e tests ===\n")

run_test("use + count", {
    con <- fresh_conn()
    dbExecute(con, sprintf('use "%s", clear', data_path))
    result <- dbGetQuery(con, "count")
    stopifnot(result[[1]] == 5)
    dbDisconnect(con, shutdown = TRUE)
})

run_test("keep if + count", {
    con <- fresh_conn()
    dbExecute(con, sprintf('use "%s", clear', data_path))
    dbExecute(con, "keep if year == 2018")
    result <- dbGetQuery(con, "count")
    stopifnot(result[[1]] == 3)
    dbDisconnect(con, shutdown = TRUE)
})

run_test("list after keep", {
    con <- fresh_conn()
    dbExecute(con, sprintf('use "%s", clear', data_path))
    dbExecute(con, "keep if year == 2018")
    result <- dbGetQuery(con, "list")
    stopifnot("Beta" %in% result$name)
    stopifnot(!("Acme" %in% result$name))
    dbDisconnect(con, shutdown = TRUE)
})

run_test("generate", {
    con <- fresh_conn()
    dbExecute(con, sprintf('use "%s", clear', data_path))
    dbExecute(con, "generate double_revenue = revenue * 2")
    dbExecute(con, "keep if id == 1")
    result <- dbGetQuery(con, "list")
    stopifnot(result$double_revenue[1] == 2000)
    dbDisconnect(con, shutdown = TRUE)
})

run_test("describe", {
    con <- fresh_conn()
    dbExecute(con, sprintf('use "%s", clear', data_path))
    result <- dbGetQuery(con, "describe")
    stopifnot("revenue" %in% result[[1]])
    dbDisconnect(con, shutdown = TRUE)
})

run_test("summarize", {
    con <- fresh_conn()
    dbExecute(con, sprintf('use "%s", clear', data_path))
    result <- dbGetQuery(con, "summarize revenue")
    stopifnot(result$mean[1] == 1600.0)
    dbDisconnect(con, shutdown = TRUE)
})

run_test("inline data + use table", {
    con <- fresh_conn()
    dbExecute(con, "CREATE TABLE t AS SELECT 2020 AS year, 1 AS x UNION ALL SELECT 2021 AS year, 2 AS x")
    dbExecute(con, "use t")
    dbExecute(con, "keep if year == 2020")
    result <- dbGetQuery(con, "list")
    stopifnot(nrow(result) == 1)
    stopifnot(result$x[1] == 1)
    dbDisconnect(con, shutdown = TRUE)
})

if (failures > 0) {
    cat(sprintf("=== %d test(s) FAILED ===\n", failures))
    quit(status = 1)
}

cat("=== All R tests passed ===\n")
