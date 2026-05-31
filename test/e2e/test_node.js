// E2E test: Node.js duckdb client with dodo extension
const duckdb = require("duckdb");
const path = require("path");
const fs = require("fs");

const PROJECT_DIR = path.resolve(__dirname, "..", "..");
const EXT_PATH = path.join(
  PROJECT_DIR,
  "build",
  "release",
  "extension",
  "dodo",
  "dodo.duckdb_extension",
);
const DATA_PATH = path.join(PROJECT_DIR, "test", "data", "firms.csv");

if (!fs.existsSync(EXT_PATH)) {
  console.log(`FAIL: Extension not found at ${EXT_PATH}`);
  process.exit(1);
}

let failures = 0;

function freshConn() {
  return new Promise((resolve, reject) => {
    const db = new duckdb.Database(":memory:", {
      allow_unsigned_extensions: "true",
    });
    const con = db.connect();
    con.run(`LOAD '${EXT_PATH}'`, (err) => {
      if (err) reject(err);
      else resolve({ db, con });
    });
  });
}

function exec(con, sql) {
  return new Promise((resolve, reject) => {
    con.run(sql, (err) => {
      if (err) reject(err);
      else resolve();
    });
  });
}

function query(con, sql) {
  return new Promise((resolve, reject) => {
    con.all(sql, (err, rows) => {
      if (err) reject(err);
      else resolve(rows);
    });
  });
}

async function runTest(name, fn) {
  try {
    await fn();
    console.log(`  PASS: ${name}`);
  } catch (e) {
    console.log(`  FAIL: ${name}`);
    console.log(`    ${e.message}`);
    failures++;
  }
}

function assert(condition, msg) {
  if (!condition) throw new Error(msg);
}

async function main() {
  console.log("=== Node.js e2e tests ===");

  await runTest("use + count", async () => {
    const { con } = await freshConn();
    await exec(con, `use "${DATA_PATH}", clear`);
    const rows = await query(con, "count");
    assert(
      Object.values(rows[0])[0] === 5,
      `expected 5, got ${Object.values(rows[0])[0]}`,
    );
  });

  await runTest("keep if + count", async () => {
    const { con } = await freshConn();
    await exec(con, `use "${DATA_PATH}", clear`);
    await exec(con, "keep if year == 2018");
    const rows = await query(con, "count");
    assert(
      Object.values(rows[0])[0] === 3,
      `expected 3, got ${Object.values(rows[0])[0]}`,
    );
  });

  await runTest("list after keep", async () => {
    const { con } = await freshConn();
    await exec(con, `use "${DATA_PATH}", clear`);
    await exec(con, "keep if year == 2018");
    const rows = await query(con, "list");
    const names = rows.map((r) => r.name);
    assert(names.includes("Beta"), `expected Beta in ${names}`);
    assert(!names.includes("Acme"), `Acme should be filtered out`);
  });

  await runTest("generate", async () => {
    const { con } = await freshConn();
    await exec(con, `use "${DATA_PATH}", clear`);
    await exec(con, "generate double_revenue = revenue * 2");
    await exec(con, "keep if id == 1");
    const rows = await query(con, "list");
    assert(
      rows[0].double_revenue === 2000,
      `expected 2000, got ${rows[0].double_revenue}`,
    );
  });

  await runTest("describe", async () => {
    const { con } = await freshConn();
    await exec(con, `use "${DATA_PATH}", clear`);
    const rows = await query(con, "describe");
    const colNames = rows.map((r) => Object.values(r)[0]);
    assert(colNames.includes("revenue"), `expected revenue in ${colNames}`);
  });

  await runTest("summarize", async () => {
    const { con } = await freshConn();
    await exec(con, `use "${DATA_PATH}", clear`);
    const rows = await query(con, "summarize revenue");
    assert(rows[0].mean === 1600.0, `expected mean 1600, got ${rows[0].mean}`);
  });

  await runTest("inline data + use table", async () => {
    const { con } = await freshConn();
    await exec(
      con,
      "CREATE TABLE t AS SELECT 2020 AS year, 1 AS x UNION ALL SELECT 2021 AS year, 2 AS x",
    );
    await exec(con, "use t");
    await exec(con, "keep if year == 2020");
    const rows = await query(con, "list");
    assert(rows.length === 1, `expected 1 row, got ${rows.length}`);
    assert(rows[0].x === 1, `expected x=1, got ${rows[0].x}`);
  });

  if (failures > 0) {
    console.log(`=== ${failures} test(s) FAILED ===`);
    process.exit(1);
  }

  console.log("=== All Node.js tests passed ===");
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
