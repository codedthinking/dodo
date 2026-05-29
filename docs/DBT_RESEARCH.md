# Pipeline Execution Research Notes

Research for dodo's pipeline execution model. Sources: dbt docs (v1.13), "Build Systems à la Carte" (Mokhov, Mitchell, Peyton Jones 2018), dataflow programming, Petri nets, Make, Snakemake, incremental computing. No decisions made yet — this is a survey of concepts and prior art.

---

## 1. The bipartite DAG model

The pipeline is a bipartite directed acyclic graph with two kinds of nodes:

- **Data nodes** — tables, views, CTEs, scalar values. These are the edges' payloads. They can be sources (external inputs), sinks (final outputs), or intermediates.
- **Code nodes** — one or more dodo commands that consume input data nodes and produce output data nodes.

Edges always go data → code or code → data, never data → data or code → code. This is exactly the structure of a **Petri net** (places = data, transitions = code, tokens = the actual data values).

### Petri net properties that apply

- **Firing rule**: a code node can execute when all its input data nodes have values (all tokens present). This is the data-driven scheduling rule.
- **Token consumption/production**: executing a code node consumes its input tokens and produces output tokens. In our case, "consumption" may be read-only (the input data persists) or destructive (the input CTE is subsumed).
- **Marking**: the current state of the pipeline is which data nodes have been computed. This is the checkpoint/snapshot concept.
- **Boundedness**: each data node holds at most one value (one table/result). This simplifies the model vs general Petri nets.
- **Liveness**: every code node should eventually be fireable given the right inputs. A dead transition means unreachable code — a pipeline design error.

### Why bipartite matters

The bipartite structure enforces that data and computation are distinct concerns. A data node has:
- A value (or not yet computed)
- A schema/type
- A materialization strategy (CTE, view, table, file)
- A hash/fingerprint for change detection

A code node has:
- Input data dependencies
- Output data products
- The transformation logic (dodo commands)
- Possible side effects (logging, file I/O, errors)
- Execution cost (time, memory)

Keeping these separate means materialization strategy is a property of data nodes, not code nodes. The user thinks about transformations; the system decides how to store intermediates.

---

## 2. Build systems taxonomy (Mokhov et al. 2018)

The key insight: every build system is a composition of two independent choices:

### Scheduler (execution order)

| Scheduler | How it works | Dependencies | Systems |
|-----------|-------------|--------------|---------|
| **Topological** | Pre-compute full execution order from static dependency graph | Static only | Make, CloudBuild, Buck |
| **Restarting** | Start executing in some order; abort and restart if a needed dependency isn't ready | Dynamic | Excel, Bazel |
| **Suspending** | Start executing; suspend when a dependency is needed; resume when it's ready | Dynamic | Shake, Nix |

**Static vs dynamic dependencies**: In Make, all dependencies are known before execution starts (from the Makefile). In Excel, a cell's formula may reference different cells depending on values (`INDIRECT`), so dependencies are discovered during execution. Dodo's dependencies are mostly static (the commands in a `.do` file are known), but `cond()`-based logic could create data-dependent paths.

### Rebuilder (what to re-execute)

| Rebuilder | What it stores | Minimal? | Early cutoff? |
|-----------|---------------|----------|---------------|
| **Dirty bit** | One bit per key (clean/dirty) | Yes | No |
| **Verifying traces** | Hash of each dependency from last build | Yes | Yes |
| **Constructive traces** | Hash + the actual result value | Yes (cloud) | Yes |
| **Deep constructive traces** | Hash of terminal inputs only (skip intermediates) | Yes (cloud) | No |

**Early cutoff**: if a code node re-executes but produces the same output as before, downstream nodes don't need rebuilding. Verifying traces support this; dirty bits don't (once dirty, everything downstream is dirty too).

### The full taxonomy (Table 2 from the paper)

|                          | Topological | Restarting | Suspending |
|--------------------------|-------------|------------|------------|
| Dirty bit                | Make        | Excel      | —          |
| Verifying traces         | Ninja       | —          | Shake      |
| Constructive traces      | CloudBuild  | Bazel      | —          |
| Deep constructive traces | Buck        | —          | Nix        |

### Core abstractions

**Store**: a key → value mapping. Keys are node identifiers. Values are the computed results (tables, files). The store also holds persistent build information (hashes, timestamps, dependency graphs from previous runs).

**Task**: a function that, given a way to `fetch` the values of its dependencies, computes a new value. The task uses a callback to request dependency values — this is how the scheduler discovers what a task needs.

**Build**: takes a task description, a target key, and a store; returns an updated store where the target and its dependencies are up to date.

**Correctness**: a build result is correct if (1) inputs are unchanged (no corruption during build) and (2) every output equals what you'd get by recomputing its task from scratch with the final store.

**Minimality**: a build system is minimal if it executes tasks at most once per build, and only if they transitively depend on changed inputs.

---

## 3. Dataflow programming

The pipeline-as-graph model is an instance of dataflow programming:

- Operations are **black boxes** with explicit inputs and outputs
- An operation **fires when all inputs are valid** (data-driven, not control-driven)
- Inherently parallel: independent operations can execute concurrently
- The graph structure makes dependencies explicit and visible

Key difference from imperative `.do` files: dataflow is declarative (the graph determines order), while `.do` files are imperative (the user writes commands in sequence). The bipartite DAG model bridges this: the user writes commands sequentially, but the system extracts the dataflow graph and can reorder/parallelize.

---

## 4. Make / Snakemake patterns

### Make

- **target: prerequisites → recipe**: the fundamental dependency rule
- **Timestamp-based invalidation**: rebuild if any prerequisite is newer than the target
- **Topological execution**: process the DAG bottom-up
- **Parallel execution**: `make -j N` runs independent targets concurrently
- **Partial rebuild**: only rebuild what's affected by changes

### Snakemake

Extends Make's model for data pipelines:
- **Input/output declarations** on rules (not just filenames)
- **Wildcard patterns** for generalizing rules across datasets
- **Checksum-based invalidation** for small files (not just timestamps)
- **`ancient()` and `protected()`** markers to control rebuild behavior
- **Retries with escalating resources** (more memory on retry)

---

## 5. Incremental computing

The general problem: given a computation and a small change to its inputs, recompute only the affected outputs.

- **Change propagation**: follow the transitive closure of dependencies from changed inputs to affected outputs
- **Self-adjusting computation**: automatically track which values depend on which inputs at runtime, cache stable subcomputations
- **Static methods**: analyze the program before execution to derive an incremental version
- **Dynamic methods**: record dependency information during execution, use it for selective re-execution on the next run

Spreadsheets are the canonical example: change a cell, recalculate only the cells that depend on it (transitively).

---

## 6. Memoization and caching

- **Memoization** stores results of pure functions to avoid recomputation with the same inputs
- Only valid for **referentially transparent** computations (same inputs → same output)
- **Side effects break memoization**: if a code node logs, writes files, or depends on external state, its result can't be safely cached based on input hashes alone
- **Content-addressable storage** (Bazel, Nix): index cached results by the hash of their inputs. If anyone has ever computed this exact input combination, reuse the result.
- **Time-space tradeoff**: storing intermediate results costs storage but saves recomputation time. The right balance depends on how expensive the computation is vs how much storage costs.

---

## 7. State tracking and artifacts

### dbt approach

- **manifest.json**: full snapshot of the project — all nodes, their configurations, `parent_map` and `child_map` for DAG traversal
- **run_results.json**: per-node execution results — status, timing (compile + execute phases), thread ID, compiled SQL, adapter response
- **State comparison**: `--defer` and `state:modified` compare current project against a previous manifest to determine what changed, enabling "slim CI" (run only modified models against production data)
- **Retry**: `dbt retry` resumes from the point of failure using run_results.json

### General patterns

Any pipeline executor needs to track:
1. **What was computed** (which nodes have values in the store)
2. **When it was computed** (timestamps or version numbers)
3. **What inputs were used** (dependency hashes / verifying traces)
4. **Whether the result is still valid** (compare current input hashes to recorded ones)
5. **What failed and why** (error messages, which node, which inputs)

---

## 8. Error handling and precious intermediates

### Error propagation in DAGs

When a code node errors:
- **Stop downstream**: all nodes that transitively depend on the failed node are skipped (Airflow calls this `upstream_failed`)
- **Continue independent branches**: nodes on unrelated branches can still execute
- **Retry with escalation**: Snakemake retries with more resources; Airflow retries with configurable delays

### Precious intermediates

When a code node is expensive (slow), its output data nodes become "precious" — worth persisting to avoid recomputation:
- **Automatic materialization**: if a node took > N seconds, materialize it as a table instead of keeping it as a CTE
- **Checkpoint/snapshot**: the user explicitly marks a data node as worth preserving (dodo's `preserve` is a limited version of this)
- **Protected outputs**: Snakemake's `protected()` prevents accidental deletion of expensive results

### Side effects

Code nodes may have side effects beyond producing data:
- **File I/O**: `save`, `export` write to disk — these are sinks in the DAG
- **Logging**: recording what happened (audit trail, history)
- **Schema changes**: `CREATE TABLE`, `DROP TABLE` — DDL side effects
- **External API calls**: not relevant now but could be in the future

Ideally, side effects are modeled as separate sink nodes in the DAG, not entangled with data transformations. This keeps the core graph pure (memoizable) and makes side effects explicit and controllable.

---

## 9. Relevance to dodo's bipartite DAG

### What maps from existing systems

| Concept | Source | Dodo equivalent |
|---------|--------|-----------------|
| Bipartite graph (places/transitions) | Petri nets | Data nodes / code nodes |
| Firing rule (execute when inputs ready) | Petri nets, dataflow | Topological execution of the DAG |
| Store (key → value) | Build Systems à la Carte | Data nodes holding table/CTE/view values |
| Task (fetch callback for dependencies) | Build Systems à la Carte | Code node requesting input data |
| Scheduler (topological/suspending) | Build Systems à la Carte | Execution order strategy |
| Rebuilder (dirty bit/verifying traces) | Build Systems à la Carte | What to re-execute on change |
| Materialization (table/view/CTE/ephemeral) | dbt | How data nodes are persisted |
| Early cutoff | Shake, dbt | Skip downstream if output unchanged |
| `ref()` / `source()` | dbt | How code nodes reference data nodes |
| Manifest + run results | dbt | Pipeline state between runs |
| Timestamp/checksum invalidation | Make, Snakemake | When to rebuild |
| Content-addressable cache | Bazel, Nix | Share computed results |

### Open questions (no decisions yet)

1. **Scheduler choice**: topological (static, like Make) or suspending (dynamic, like Shake)? Dodo commands have mostly static dependencies, suggesting topological. But conditional logic (`cond()`, `if`) could create data-dependent paths.

2. **Rebuilder choice**: dirty bits (simple, like Make) or verifying traces (hash-based, supports early cutoff, like Shake)? Hash-based is more correct but requires hashing data nodes, which could be expensive for large tables.

3. **Materialization strategy**: which data nodes are CTEs (ephemeral), which are views, which are tables? Should this be automatic (performance-driven) or user-controlled? The paper suggests this should be transparent — the system decides based on cost/benefit.

4. **Granularity**: is each dodo command a separate code node, or is a sequence of commands grouped into one code node? Finer granularity enables more precise invalidation but creates a larger graph.

5. **Side effect isolation**: how to handle `save`, `export`, logging? Model them as separate sink nodes? Or as annotations on code nodes?

6. **Error recovery**: when a code node fails, what state is preserved? Can we resume from the last successful data node? This interacts with materialization — ephemeral CTEs can't be checkpointed.

7. **Interactive vs batch**: the bipartite DAG must support both interactive exploration (edit one node, see results) and batch execution (run the whole pipeline). Interactive mode wants fast feedback; batch mode wants minimal recomputation.

8. **State persistence**: what to store between runs? Minimal (just hashes) or constructive (actual result values)? The paper shows these are independent choices with different tradeoffs.

9. **Multi-file pipelines**: how do `.do` files compose into a larger DAG? Each file could be one code node, or multiple files could share data nodes via `save`/`use` pairs. The `tempfile` mechanism already hints at this.

10. **Topological ordering vs user ordering**: the DAG defines a partial order. The user writes commands in total order (top to bottom in a `.do` file). The system needs to respect the DAG's partial order for correctness but can reorder for performance. The user should never need to think about this.
