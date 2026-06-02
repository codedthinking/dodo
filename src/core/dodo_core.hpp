#pragma once

#include "string_utils.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dodo {

//===--------------------------------------------------------------------===//
// FNV-1a hash → 12-char hex string (dependency-free, for content addressing)
//===--------------------------------------------------------------------===//
inline std::string HashString(const std::string &input) {
	constexpr uint64_t FNV_OFFSET = 14695981039346656037ULL;
	constexpr uint64_t FNV_PRIME = 1099511628211ULL;
	uint64_t hash = FNV_OFFSET;
	for (unsigned char c : input) {
		hash ^= c;
		hash *= FNV_PRIME;
	}
	char buf[13];
	snprintf(buf, sizeof(buf), "%012llx", (unsigned long long)(hash & 0xFFFFFFFFFFFFULL));
	return std::string(buf);
}

//===--------------------------------------------------------------------===//
// SymbolEntry — unified symbol table entry (LITERAL text or VARIABLE ref)
//===--------------------------------------------------------------------===//
enum class SymbolKind { LITERAL, VARIABLE };

struct SymbolEntry {
	SymbolKind kind;
	std::string value; //! LITERAL: the text; VARIABLE: the DuckDB variable name (_dodo_l_x etc.)
};

//===--------------------------------------------------------------------===//
// DodoCommand — parsed .do command representation
//===--------------------------------------------------------------------===//
struct DodoCommand {
	std::string command;
	std::string arguments;
	std::string condition;
	std::string options;
	std::string bysort_partition; // PARTITION BY vars, comma-separated
	std::string bysort_order;     // ORDER BY vars, comma-separated
};

//===--------------------------------------------------------------------===//
// DodoState — CTE chain state for .do file to SQL translation
//===--------------------------------------------------------------------===//
struct DodoState {
	std::vector<std::string> cte_steps;        //! Inner SQL for each CTE step (without _sN AS wrapper)
	std::vector<std::string> cte_commands;     //! Original command text for each step
	int step_counter = 0;
	std::string current_source;
	std::string pending_command; //! Set before ProcessCommand, consumed by AddStep

	//! SQL formatting options
	bool format_sql = true;
	bool sql_comments = true;

	//! Redo stack: (command_text, inner_sql) pairs popped by undo
	std::vector<std::pair<std::string, std::string>> redo_stack;

	//! Variable labels: column_name -> label text
	std::unordered_map<std::string, std::string> variable_labels;
	//! Value label definitions: label_name -> {value -> text}
	std::unordered_map<std::string, std::unordered_map<int, std::string>> value_label_defs;
	//! Column-to-value-label mapping: column_name -> label_name
	std::unordered_map<std::string, std::string> column_labels;

	//! Unified symbol tables — four namespaces, each name → SymbolEntry
	std::unordered_map<std::string, SymbolEntry> local_symbols;
	std::unordered_map<std::string, SymbolEntry> global_symbols;
	std::unordered_map<std::string, SymbolEntry> scalar_symbols;

	//! Pending SET VARIABLE SQL to emit (drained by ProcessLines / extension)
	std::vector<std::string> pending_sql;

	//! Tempvar/tempname tracking
	std::vector<std::string> tempvar_columns;  //! columns to exclude at scope end
	std::vector<std::string> tempname_names;   //! scalar/macro names to drop at scope end
	int temp_counter = 0;                      //! unique name generator

	//! Tempfile names (registered via tempfile command)
	std::unordered_set<std::string> tempfile_names;
	//! Whether _tempfiles schema has been created
	bool tempfiles_schema_created = false;
	//! Tempfile tables created (for garbage collection on clear)
	std::vector<std::string> tempfile_tables;

	//! Panel structure (set by xtset/tsset)
	std::string panel_var; // empty if pure time-series
	std::string time_var;  // empty if not set

	//! Whether dodo._current table exists (materialized use)
	bool materialized = false;

	//! Whether live view is enabled (set by extension, used by clear)
	bool live_view_enabled = false;

	//! Materialization checkpoint state
	std::string current_node_hash;                     //! Hash of current materialized node table
	std::vector<std::string> accumulated_commands;     //! Command history across checkpoints
	std::unordered_set<std::string> node_tables;       //! Tracked __node_<hash> tables for cleanup

	//! Preserve checkpoint: index into cte_steps (-1 = no active preserve)
	int preserve_checkpoint = -1;
	int preserve_step_counter = -1;
	std::string preserve_node_hash;

	//! Generate a unique DuckDB variable name for SET VARIABLE
	static std::string GenerateUname(const std::string &prefix, const std::string &name) {
		return "_dodo_" + prefix + "_" + name;
	}

	//! Resolve a symbol entry for substitution
	static std::string ResolveSymbol(const SymbolEntry &entry) {
		if (entry.kind == SymbolKind::LITERAL) {
			return entry.value;
		}
		return "getvariable('" + entry.value + "')";
	}

	std::string LatestStep() const {
		return cte_steps.empty() ? "" : "_s" + std::to_string(step_counter - 1);
	}

	bool HasData() const {
		return !cte_steps.empty();
	}

	void AddStep(const std::string &inner_sql) {
		cte_steps.push_back(inner_sql);
		cte_commands.push_back(pending_command);
		step_counter++;
		// New step invalidates redo history
		redo_stack.clear();
	}

	//! Build WITH ... AS ... prefix (defined in .cpp for formatting support)
	std::string BuildCTEPrefix() const;
	//! Build full query: CTE prefix + final SELECT
	std::string BuildQuery(const std::string &final_select) const;

	//! Returns true if CTE chain has transformation steps beyond the base
	bool NeedsCheckpoint() const {
		return materialized && cte_steps.size() > 1;
	}

	//! Flush CTE chain after materialization: save history, restart from VIEW
	void FlushCTEChain() {
		for (auto &cmd : cte_commands) {
			accumulated_commands.push_back(cmd);
		}
		cte_steps.clear();
		cte_commands.clear();
		redo_stack.clear();
		cte_steps.push_back("SELECT * FROM dodo._current");
		cte_commands.push_back("");
		step_counter = 1;
	}

	//! Materialize CTE chain as a checkpoint table, flush, and restart
	void MaybeCheckpoint() {
		if (!NeedsCheckpoint()) {
			return;
		}
		std::string full_query = BuildQuery("SELECT * FROM " + LatestStep());
		std::string hash = HashString(current_node_hash + std::string("\0", 1) + full_query);
		std::string node_name = "dodo.__node_" + hash;

		pending_sql.push_back("CREATE TABLE IF NOT EXISTS " + node_name + " AS (" + full_query + ")");
		EmitColumnComments(node_name);
		pending_sql.push_back("CREATE OR REPLACE VIEW dodo._current AS SELECT * FROM " + node_name);

		current_node_hash = hash;
		node_tables.insert(node_name);
		FlushCTEChain();
	}

	//! Emit COMMENT ON COLUMN for all tracked variable labels
	void EmitColumnComments(const std::string &table_name) {
		for (auto &[col, label] : variable_labels) {
			std::string escaped = label;
			size_t pos = 0;
			while ((pos = escaped.find('\'', pos)) != std::string::npos) {
				escaped.replace(pos, 1, "''");
				pos += 2;
			}
			pending_sql.push_back("COMMENT ON COLUMN " + table_name + "." + col + " IS '" + escaped + "'");
		}
	}

	//! Get SQL to drop tempfile tables, materialized table, and schemas
	std::string BuildCleanupSQL() const {
		std::string sql;
		for (auto &tbl : tempfile_tables) {
			sql += "DROP TABLE IF EXISTS " + tbl + "; ";
		}
		if (tempfiles_schema_created) {
			sql += "DROP SCHEMA IF EXISTS _tempfiles CASCADE; ";
		}
		if (materialized) {
			sql += "DROP TABLE IF EXISTS dodo._history; ";
			sql += "DROP VIEW IF EXISTS dodo._current; ";
			for (auto &node : node_tables) {
				sql += "DROP TABLE IF EXISTS " + node + "; ";
			}
			sql += "DROP SCHEMA IF EXISTS dodo CASCADE; ";
		}
		return sql;
	}

	void Clear() {
		cte_steps.clear();
		cte_commands.clear();
		redo_stack.clear();
		step_counter = 0;
		current_source.clear();
		pending_command.clear();
		variable_labels.clear();
		value_label_defs.clear();
		column_labels.clear();
		// Macros, scalars, and tempnames persist across clear (Stata behavior)
		tempvar_columns.clear();
		tempfile_names.clear();
		preserve_checkpoint = -1;
		preserve_step_counter = -1;
		materialized = false;
		current_node_hash.clear();
		accumulated_commands.clear();
		node_tables.clear();
		preserve_node_hash.clear();
	}

	//! Full reset: also clears macros, scalars, and tempnames
	void ClearMacros() {
		local_symbols.clear();
		global_symbols.clear();
		scalar_symbols.clear();
		tempname_names.clear();
		temp_counter = 0;
	}

	void ClearAll() {
		Clear();
		tempfile_tables.clear();
		tempfiles_schema_created = false;
	}
};

//===--------------------------------------------------------------------===//
// Free function declarations
//===--------------------------------------------------------------------===//

//! List of recognized dodo commands
extern const std::vector<std::string> DODO_COMMANDS;

//! Check if a query string is a dodo command; if so, set command_out
bool IsDodoCommand(const std::string &query, std::string &command_out);

//! Returns true if the command transforms data (adds CTE step)
bool IsTransformationCommand(const std::string &command);

//! Returns true if the command has side effects (e.g., save, export)
bool IsSideEffectCommand(const std::string &command);

//! Parse a raw .do command string into structured DodoCommand
DodoCommand TokenizeCommand(const std::string &query);

//! Translate a Stata expression to SQL
std::string TranslateExpression(const std::string &expr, const std::string &by_cols = "",
                                const std::string &panel_var = "", const std::string &time_var = "",
                                const std::string &bysort_order = "");

//! Expand Stata macros in text: `name' for locals, $name/${name} for globals, scalar(name)
std::string ExpandMacros(const std::string &text, const DodoState &state);

//! Evaluate a simple numeric expression (for local x = expr, scalar x = expr)
double EvaluateSimpleExpr(const std::string &expr);

//! Evaluate a macro function (e.g., :variable label varname, :word count string)
std::string EvaluateMacroFunction(const std::string &func, const DodoState &state);

//! Parse a Stata numlist specification (e.g., "1/5", "1(2)10")
std::vector<std::string> ParseNumlist(const std::string &spec);

//! Line reader callback: returns true and fills line, or false at EOF
using LineReader = std::function<bool(std::string &line)>;

//! Process lines from a source, returning side-effect SQL statements
std::vector<std::string> ProcessLines(LineReader reader, DodoState &state, bool skip_terminal);

//! Process a single parsed command, returning SQL (or empty string)
std::string ProcessCommand(const DodoCommand &cmd, DodoState &state);

//! Process an entire .do file, returning a vector of SQL statements
std::vector<std::string> ProcessDoFile(const std::string &filename, DodoState &state);

//! Extract a quoted string (strip surrounding quotes)
std::string ExtractQuotedString(const std::string &s);

//! Return the appropriate DuckDB read function for a file extension
std::string FileReadFunction(const std::string &filename);

} // namespace dodo
