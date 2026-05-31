# Plan: `write_dta` Module

## Context

The extension can read .dta files but has no native .dta writer. Currently, `save "file.dta"` falls through to `COPY ... TO 'file.dta'` without a format clause, which fails because DuckDB has no built-in .dta writer. We'll implement a DuckDB `CopyFunction` named `"dta"` that writes format-119 .dta files, enabling both:

```sql
COPY my_table TO 'output.dta' (FORMAT dta);
```

and automatic detection in the `save` command:

```stata
save "output.dta"
```

## File Structure

```
src/write_dta/
  dta_writer.hpp      -- DtaWriter class (pure C++17, no DuckDB dependency)
  dta_writer.cpp      -- Implementation: header, map, metadata, data, value labels
  write_dta_function.hpp  -- DuckDB CopyFunction declarations
  write_dta_function.cpp  -- bind/init/sink/combine/finalize callbacks
src/include/
  dta_writer.hpp      -- proxy header
```

## Architecture

### Layer 1: `DtaWriter` (pure C++17, no DuckDB)

A stateful writer class that builds a format-119 .dta file sequentially.

```cpp
struct DtaWriteColumn {
    std::string name;           // max 32 UTF-8 chars (129 bytes)
    uint16_t type_code;         // 1-2045=str, 32768=strL, 65526-65530=numeric
    uint16_t byte_width;        // bytes per observation for this variable
    std::string format;         // display format (e.g. "%9.0g", "%td")
    std::string value_label_name;
    std::string label;          // variable label (max 80 UTF-8 chars, 321 bytes)
};

class DtaWriter {
public:
    DtaWriter(const std::string &path, const std::vector<DtaWriteColumn> &columns,
              const std::string &dataset_label = "");
    ~DtaWriter();

    // Write observations row by row (buffered internally)
    void BeginData();
    void WriteRow(const std::vector<const char *> &raw_values);
    // Or column-oriented: write a chunk of rows from pre-transposed buffers
    void WriteRows(const char *row_buffer, uint64_t n_rows);

    // StrL support
    void AddStrL(uint32_t var_index, uint64_t obs_index, const std::string &value);

    // Value labels
    void AddValueLabel(const std::string &name,
                       const std::unordered_map<int32_t, std::string> &mappings);

    // Finalize: write strls, value labels, map, close file
    void Finalize(uint64_t total_obs);

private:
    FILE *fp_;
    std::vector<DtaWriteColumn> columns_;
    std::string dataset_label_;
    uint32_t row_width_;
    uint64_t n_obs_;

    // File positions for <map> (written at finalize)
    uint64_t map_offsets_[14];

    // StrL accumulator
    struct StrLEntry { uint32_t v; uint64_t o; std::string value; };
    std::vector<StrLEntry> strl_entries_;

    // Value labels
    struct ValueLabelDef { std::string name; std::unordered_map<int32_t, std::string> mappings; };
    std::vector<ValueLabelDef> value_labels_;

    void WriteHeader();
    void WriteMap();         // placeholder, rewritten at Finalize
    void WriteVariableTypes();
    void WriteVarnames();
    void WriteSortlist();
    void WriteFormats();
    void WriteValueLabelNames();
    void WriteVariableLabels();
    void WriteCharacteristics();
    void WriteStrLs();
    void WriteValueLabels();
    void WriteTag(const char *tag);

    template<typename T> T ToByteOrder(T val) const;
};
```

Format-119 specifics:
- `<release>119</release>`, `<byteorder>LSF</byteorder>` (native x86/ARM)
- K field: 4 bytes (supports >32,767 variables)
- N field: 8 bytes
- Variable names: 129 bytes each
- Format strings: 57 bytes each
- Value-label names: 129 bytes each
- Variable labels: 321 bytes each
- Sortlist entries: 4 bytes each
- Dataset label length: 2 bytes
- No alias variables

### Layer 2: DuckDB CopyFunction (`write_dta_function.cpp`)

Registers a `CopyFunction` named `"dta"` via `loader.RegisterFunction()`.

#### Bind (`WriteDtaBind`)

Receives column names and types from DuckDB. Maps DuckDB types to .dta types:

| DuckDB type | .dta type | type_code | format |
|---|---|---|---|
| TINYINT | byte | 65530 | %8.0g |
| SMALLINT | int | 65529 | %8.0g |
| INTEGER | long | 65528 | %12.0g |
| BIGINT | double | 65526 | %10.0g |
| FLOAT | float | 65527 | %9.0g |
| DOUBLE | double | 65526 | %10.0g |
| BOOLEAN | byte | 65530 | %8.0g |
| DATE | double | 65526 | %td |
| TIMESTAMP | double | 65526 | %tc |
| VARCHAR | str# or strL | 1-2045 or 32768 | %#s |
| ENUM | byte/int/long | depends on size | %8.0g |
| Other | VARCHAR (cast) | str# or strL | %#s |

Bind data stores:
- Column metadata (names, types, formats)
- Whether to use strL for strings (option or auto-detect)
- Value label definitions (from ENUM types)

#### Options

| Option | Type | Default | Description |
|---|---|---|---|
| `version` | INTEGER | 119 | .dta format version (118 or 119) |
| `variable_labels` | STRUCT | {} | Map of column name → label |

#### Global State (`WriteDtaInitializeGlobal`)

Opens the file, creates `DtaWriter`, writes header sections (everything up to `<data>`). Stores file handle and writer.

#### Sink (`WriteDtaSink`)

For each DataChunk:
1. Column-to-row transpose: for each row in the chunk, pack all column values into the row-major .dta format
2. Handle type conversions:
   - DATE → days since 1960-01-01 (add 3653 to DuckDB epoch)
   - TIMESTAMP → milliseconds since 1960-01-01
   - NULL → appropriate missing value sentinel per type
   - ENUM → integer value (stored as byte/int/long depending on enum size)
   - VARCHAR → fixed-width string (pad/truncate) or strL reference
3. Write packed rows to file
4. Accumulate strL entries for later

Thread safety: use a mutex (single-threaded write, like the blob copy function).

#### Combine (`WriteDtaCombine`)

No-op (single-threaded).

#### Finalize (`WriteDtaFinalize`)

1. Write `</data>` tag
2. Write `<strls>...</strls>` (accumulated strL entries)
3. Write `<value_labels>...</value_labels>` (from ENUM types)
4. Write `</stata_dta>`
5. Seek back to `<map>` and rewrite with actual file positions
6. Close file

### String Handling Strategy

For VARCHAR columns, the bind phase must decide between str# (fixed-width) and strL:

1. **Default**: Use strL (type 32768) for all VARCHAR columns. This avoids the need to scan all data to determine max string length, and handles arbitrarily long strings.
2. **Optimization** (future): A first-pass scan could determine max byte length per column and use str# if all values fit in 2045 bytes.

## Build System Changes

`CMakeLists.txt`:
```cmake
set(EXTENSION_SOURCES
    src/extension/dodo_extension.cpp
    src/core/dodo_core.cpp
    src/read_dta/dta_reader.cpp
    src/read_dta/read_dta_function.cpp
    src/write_dta/dta_writer.cpp
    src/write_dta/write_dta_function.cpp)
```

## Integration Points

### 1. Extension registration (`dodo_extension.cpp`)

In `LoadInternal()`:
```cpp
// Register COPY TO dta format
CopyFunction dta_copy = GetDtaCopyFunction();
loader.RegisterFunction(dta_copy);
```

This enables `COPY ... TO 'file.dta' (FORMAT dta)`.

### 2. Save command (`dodo_core.cpp`)

In the `save` command handler (~line 2020), add `.dta` detection:
```cpp
} else if (str::EndsWith(lower_fn, ".dta")) {
    format_clause = " (FORMAT DTA)";
}
```

### 3. Read integration

The `CopyFunction` can also set `copy_from_bind` and `copy_from_function` to wire up `read_dta` for `COPY FROM`, but this is optional — `read_dta()` already works as a table function.

## Implementation Order

1. `dta_writer.hpp` — structs, class declaration
2. `dta_writer.cpp` — header/metadata writing (all sections up to `<data>`)
3. `dta_writer.cpp` — row data writing, type conversion, missing values
4. `dta_writer.cpp` — strL writing, value labels, map rewrite, finalize
5. `write_dta_function.hpp/cpp` — DuckDB CopyFunction (bind/init/sink/finalize)
6. CMakeLists.txt + extension registration
7. Save command integration (`.dta` → `FORMAT DTA`)
8. Tests

## Verification

1. Build: `make` (or `GEN=ninja make`)
2. Test round-trip:
   ```sql
   -- Write
   COPY (SELECT 1 AS x, 'hello' AS y) TO 'test.dta' (FORMAT dta);
   -- Read back
   SELECT * FROM read_dta('test.dta');
   ```
3. Test with real data:
   ```sql
   COPY (SELECT * FROM read_dta('test/data/auto.dta')) TO 'auto_copy.dta' (FORMAT dta);
   SELECT * FROM read_dta('auto_copy.dta');
   ```
4. Verify output in Stata: `use "auto_copy.dta", clear` then `describe` and `list`
5. Test save command: `save "output.dta"`
6. Run existing test suite: `make test`
