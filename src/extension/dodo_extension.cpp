#define DUCKDB_EXTENSION_MAIN

#include "dodo_extension.hpp"
#include "regression.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/planner/binder.hpp"

namespace duckdb {

// Global pointer to shared state, used by the live_view option callback
static DodoStateInfo *g_dodo_state = nullptr;

//===--------------------------------------------------------------------===//
// Build SQL for DuckDB-interactive features (history table, live view)
//===--------------------------------------------------------------------===//

static string BuildHistorySQL(const DodoStateInfo &state) {
	if (!state.core.materialized) {
		return "";
	}
	// Combine accumulated (checkpointed) + current commands for full history
	vector<string> all_commands;
	all_commands.insert(all_commands.end(),
	                    state.core.accumulated_commands.begin(),
	                    state.core.accumulated_commands.end());
	all_commands.insert(all_commands.end(),
	                    state.core.cte_commands.begin(),
	                    state.core.cte_commands.end());

	string sql = "CREATE OR REPLACE TABLE dodo._history AS SELECT * FROM (VALUES ";
	bool first = true;
	for (idx_t i = 0; i < all_commands.size(); i++) {
		if (!first) {
			sql += ", ";
		}
		string escaped_cmd = all_commands[i];
		size_t pos = 0;
		while ((pos = escaped_cmd.find('\'', pos)) != string::npos) {
			escaped_cmd.replace(pos, 1, "''");
			pos += 2;
		}
		sql += "(" + to_string(i) + ", '" + escaped_cmd + "', false)";
		first = false;
	}
	for (idx_t i = 0; i < state.core.redo_stack.size(); i++) {
		if (!first) {
			sql += ", ";
		}
		string escaped_cmd = state.core.redo_stack[state.core.redo_stack.size() - 1 - i].first;
		size_t pos = 0;
		while ((pos = escaped_cmd.find('\'', pos)) != string::npos) {
			escaped_cmd.replace(pos, 1, "''");
			pos += 2;
		}
		int step_id = static_cast<int>(all_commands.size()) + static_cast<int>(i);
		sql += "(" + to_string(step_id) + ", '" + escaped_cmd + "', true)";
		first = false;
	}
	if (first) {
		return "CREATE OR REPLACE TABLE dodo._history (step_id INTEGER, command VARCHAR, undone BOOLEAN)";
	}
	sql += ") AS t(step_id, command, undone)";
	return sql;
}

static string BuildLiveViewSQL(const DodoStateInfo &state) {
	if (!state.live_view_enabled || !state.core.HasData()) {
		return "";
	}
	return "CREATE OR REPLACE VIEW _dodo_data AS (" +
	       state.core.BuildQuery("SELECT * FROM " + state.core.LatestStep()) + ")";
}

//===--------------------------------------------------------------------===//
// parse_function: detect commands (called when standard parser fails)
//===--------------------------------------------------------------------===//
static ParserExtensionParseResult dodo_parse(ParserExtensionInfo *info, const string &query) {
	std::string command;
	if (!dodo::IsDodoCommand(query, command)) {
		return ParserExtensionParseResult();
	}
	auto parse_data = make_uniq<DodoParseData>(query);
	return ParserExtensionParseResult(std::move(parse_data));
}

//===--------------------------------------------------------------------===//
// parser_override: handles commands that conflict with SQL keywords
//===--------------------------------------------------------------------===//
static ParserOverrideResult dodo_parser_override(ParserExtensionInfo *info, const string &query,
                                                 ParserOptions &options) {
	auto &state = dynamic_cast<DodoStateInfo &>(*info);

	// Split the full query into individual statements by ';'
	auto statements_str = StringUtil::Split(query, ';');

	bool has_dodo_commands = false;
	bool has_conflict_commands = false;
	for (auto &s : statements_str) {
		string trimmed = StringUtil::Lower(s);
		StringUtil::Trim(trimmed);
		if (trimmed.empty()) {
			continue;
		}

		if (StringUtil::StartsWith(trimmed, "use ")) {
			has_conflict_commands = true;
		}
		if (StringUtil::StartsWith(trimmed, "import ")) {
			has_conflict_commands = true;
		}
		if (StringUtil::StartsWith(trimmed, "describe") || StringUtil::StartsWith(trimmed, "summarize")) {
			has_conflict_commands = true;
		}
		if (StringUtil::StartsWith(trimmed, "reshape") && trimmed.find("wide") != string::npos) {
			has_conflict_commands = true;
		}
		// show — SQL SHOW keyword conflicts
		if (StringUtil::StartsWith(trimmed, "show") && (trimmed.size() == 4 || trimmed[4] == ' ')) {
			has_conflict_commands = true;
		}

		std::string cmd_name;
		// Need original (non-lowered) string for IsDodoCommand
		string orig = s;
		StringUtil::Trim(orig);
		if (dodo::IsDodoCommand(orig, cmd_name)) {
			has_dodo_commands = true;
		}
	}

	// Macro/loop commands always need override (they don't conflict with SQL but aren't SQL either)
	bool has_macro_commands = false;
	for (auto &s : statements_str) {
		string lower_s = StringUtil::Lower(s);
		StringUtil::Trim(lower_s);
		if (StringUtil::StartsWith(lower_s, "local ") || StringUtil::StartsWith(lower_s, "global ") ||
		    StringUtil::StartsWith(lower_s, "scalar ") || StringUtil::StartsWith(lower_s, "macro ") ||
		    StringUtil::StartsWith(lower_s, "foreach ") || StringUtil::StartsWith(lower_s, "forvalues ") ||
		    StringUtil::StartsWith(lower_s, "tempvar ") || StringUtil::StartsWith(lower_s, "tempname ") ||
		    StringUtil::StartsWith(lower_s, "display ") ||
		    StringUtil::StartsWith(lower_s, "levelsof ")) {
			has_macro_commands = true;
			break;
		}
	}

	bool any_needs_override = has_conflict_commands && (has_dodo_commands || state.HasData());
	if (!any_needs_override && (state.live_view_enabled || state.core.materialized) && has_dodo_commands) {
		any_needs_override = true;
	}
	if (!any_needs_override && has_macro_commands) {
		any_needs_override = true;
	}

	if (!any_needs_override) {
		return ParserOverrideResult();
	}

	try {
		vector<unique_ptr<SQLStatement>> all_statements;

		// Flatten all statements into individual lines (handles multi-line
		// input from do-files, pasted blocks, etc.)
		vector<string> all_lines;
		for (auto &s : statements_str) {
			auto lines = StringUtil::Split(s, '\n');
			for (auto &l : lines) {
				string tl = l;
				StringUtil::Trim(tl);
				if (!tl.empty()) {
					all_lines.push_back(tl);
				}
			}
		}

		for (idx_t li = 0; li < all_lines.size(); li++) {
			string trimmed = all_lines[li];
			if (trimmed.empty()) {
				continue;
			}

			// Expand macros before command recognition
			trimmed = dodo::ExpandMacros(trimmed, state.core);
			if (trimmed.empty()) {
				continue;
			}

			// Handle foreach/forvalues — use original (pre-expansion) text
			// to avoid corrupting loop variable references in the body
			string lower_orig = StringUtil::Lower(all_lines[li]);
			if (StringUtil::StartsWith(lower_orig, "foreach ") ||
			    StringUtil::StartsWith(lower_orig, "forvalues ")) {
				// Collect this and remaining lines for ProcessLines
				std::vector<string> loop_lines;
				for (idx_t ri = li; ri < all_lines.size(); ri++) {
					loop_lines.push_back(all_lines[ri]);
				}
				idx_t loop_idx = 0;
				dodo::LineReader loop_reader = [&](string &out) -> bool {
					if (loop_idx < loop_lines.size()) {
						out = loop_lines[loop_idx++];
						return true;
					}
					return false;
				};
				auto loop_sql = dodo::ProcessLines(loop_reader, state.core, true);
				for (auto &lsql : loop_sql) {
					if (lsql.find("SELECT 'OK' AS status") == string::npos) {
						Parser p;
						p.ParseQuery(lsql);
						for (auto &st : p.statements) {
							all_statements.push_back(std::move(st));
						}
					}
				}
				// Skip past lines consumed by the loop (up to closing })
				// ProcessLines consumed lines via the reader; loop_idx tells us how many
				li += loop_idx - 1; // -1 because the for loop increments li
				continue;
			}

			std::string cmd_name;
			if (dodo::IsDodoCommand(trimmed, cmd_name)) {
				auto cmd = dodo::TokenizeCommand(trimmed);
				state.core.pending_command = trimmed;
				string sql = dodo::ProcessCommand(cmd, state.core);

				// Handle __PIVOT__ marker
				if (StringUtil::StartsWith(sql, "__PIVOT__:")) {
					string rest = sql.substr(10);
					idx_t state_pos = rest.find("||STATE||");
					string pivot_sql = rest.substr(0, state_pos);
					string table_name = rest.substr(state_pos + 9);

					Parser parser;
					parser.ParseQuery(pivot_sql);
					state.core.AddStep("SELECT * FROM " + table_name);
					for (auto &stmt : parser.statements) {
						all_statements.push_back(std::move(stmt));
					}
					continue;
				}

				// Drain any pending SQL (SET VARIABLE from M14b)
				for (auto &psql : state.core.pending_sql) {
					Parser pend_parser;
					pend_parser.ParseQuery(psql);
					for (auto &stmt : pend_parser.statements) {
						all_statements.push_back(std::move(stmt));
					}
				}
				state.core.pending_sql.clear();

				Parser parser;
				parser.ParseQuery(sql);

				// History table and live view: inject BEFORE the last
				// statement of the command so the command result is returned
				// to clients like Python that return the last statement's result.
				string history_sql = BuildHistorySQL(state);
				string view_sql = BuildLiveViewSQL(state);

				for (idx_t si = 0; si < parser.statements.size(); si++) {
					if (si == parser.statements.size() - 1) {
						if (!history_sql.empty()) {
							Parser hist_parser;
							hist_parser.ParseQuery(history_sql);
							for (auto &stmt : hist_parser.statements) {
								all_statements.push_back(std::move(stmt));
							}
						}
						if (!view_sql.empty()) {
							Parser view_parser;
							view_parser.ParseQuery(view_sql);
							for (auto &stmt : view_parser.statements) {
								all_statements.push_back(std::move(stmt));
							}
						}
					}
					all_statements.push_back(std::move(parser.statements[si]));
				}
			} else {
				// Not a dodo command — parse as standard SQL
				Parser parser;
				parser.ParseQuery(trimmed);
				for (auto &stmt : parser.statements) {
					all_statements.push_back(std::move(stmt));
				}
			}
		}

		if (all_statements.empty()) {
			if (has_macro_commands || has_dodo_commands) {
				Parser ok_parser;
				ok_parser.ParseQuery("SELECT 'OK' AS status");
				for (auto &stmt : ok_parser.statements) {
					all_statements.push_back(std::move(stmt));
				}
			} else {
				return ParserOverrideResult();
			}
		}
		return ParserOverrideResult(std::move(all_statements));
	} catch (std::exception &ex) {
		return ParserOverrideResult(ex);
	}
}

//===--------------------------------------------------------------------===//
// plan_function: generate SQL, store parsed statement, throw to redirect
//===--------------------------------------------------------------------===//
static ParserExtensionPlanResult dodo_plan(ParserExtensionInfo *info, ClientContext &context,
                                           unique_ptr<ParserExtensionParseData> parse_data) {
	auto &dodo_data = dynamic_cast<DodoParseData &>(*parse_data);
	auto &state = dynamic_cast<DodoStateInfo &>(*info);

	auto cmd = dodo::TokenizeCommand(dodo_data.raw_query);
	state.core.pending_command = dodo_data.raw_query;
	string sql = dodo::ProcessCommand(cmd, state.core);

	Parser parser;
	try {
		parser.ParseQuery(sql);
	} catch (std::exception &ex) {
		throw BinderException("dodo: failed to parse generated SQL: %s\nSQL: %s", ex.what(), sql);
	}
	if (parser.statements.empty()) {
		throw BinderException("dodo: generated SQL produced no statements");
	}

	auto bind_state = make_shared_ptr<DodoBindState>(std::move(parser.statements[0]));
	context.registered_state->Remove("dodo_bind");
	context.registered_state->Insert("dodo_bind", bind_state);

	throw BinderException("dodo redirect to operator bind");
}

//===--------------------------------------------------------------------===//
// OperatorExtension::Bind — picks up stored statement and binds it
//===--------------------------------------------------------------------===//
BoundStatement dodo_bind(ClientContext &context, Binder &binder, OperatorExtensionInfo *info, SQLStatement &statement) {
	auto bind_state = context.registered_state->Get<DodoBindState>("dodo_bind");
	if (!bind_state || !bind_state->statement) {
		return BoundStatement();
	}

	auto sql_binder = Binder::CreateBinder(context, &binder);
	auto result = sql_binder->Bind(*bind_state->statement);

	context.registered_state->Remove("dodo_bind");

	return result;
}

//===--------------------------------------------------------------------===//
// OLS UDFs
//===--------------------------------------------------------------------===//

// dodo_ols_solve(xtx DOUBLE[], xty DOUBLE[], k INTEGER) -> DOUBLE[]
// Solves b = (X'X)^{-1} X'y via Cholesky decomposition
static void OlsSolveFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &xtx_vec = args.data[0];
	auto &xty_vec = args.data[1];
	auto &k_vec = args.data[2];

	idx_t count = args.size();
	for (idx_t i = 0; i < count; i++) {
		auto xtx_val = xtx_vec.GetValue(i);
		auto xty_val = xty_vec.GetValue(i);
		int k = k_vec.GetValue(i).GetValue<int32_t>();

		auto &xtx_children = ListValue::GetChildren(xtx_val);
		auto &xty_children = ListValue::GetChildren(xty_val);

		std::vector<double> xtx(static_cast<size_t>(k) * k);
		std::vector<double> xty(k);
		for (int j = 0; j < k * k; j++) {
			xtx[j] = xtx_children[j].GetValue<double>();
		}
		for (int j = 0; j < k; j++) {
			xty[j] = xty_children[j].GetValue<double>();
		}

		auto beta = dodo::OlsSolve(xtx, xty, k);

		vector<Value> result_values;
		for (int j = 0; j < k; j++) {
			result_values.push_back(Value::DOUBLE(beta[j]));
		}
		result.SetValue(i, Value::LIST(LogicalType::DOUBLE, std::move(result_values)));
	}
}

// dodo_ols_invdiag(xtx DOUBLE[], k INTEGER) -> DOUBLE[]
// Returns diagonal of (X'X)^{-1}
static void OlsInvDiagFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &xtx_vec = args.data[0];
	auto &k_vec = args.data[1];

	idx_t count = args.size();
	for (idx_t i = 0; i < count; i++) {
		auto xtx_val = xtx_vec.GetValue(i);
		int k = k_vec.GetValue(i).GetValue<int32_t>();

		auto &xtx_children = ListValue::GetChildren(xtx_val);
		std::vector<double> xtx(static_cast<size_t>(k) * k);
		for (int j = 0; j < k * k; j++) {
			xtx[j] = xtx_children[j].GetValue<double>();
		}

		auto diag = dodo::OlsInvDiag(xtx, k);

		vector<Value> result_values;
		for (int j = 0; j < k; j++) {
			result_values.push_back(Value::DOUBLE(diag[j]));
		}
		result.SetValue(i, Value::LIST(LogicalType::DOUBLE, std::move(result_values)));
	}
}

// dodo_sandwich_diag(xtx DOUBLE[], meat DOUBLE[], k INTEGER) -> DOUBLE[]
// Returns diagonal of (X'X)^{-1} M (X'X)^{-1} (sandwich variance)
static void SandwichDiagFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &xtx_vec = args.data[0];
	auto &meat_vec = args.data[1];
	auto &k_vec = args.data[2];

	idx_t count = args.size();
	for (idx_t i = 0; i < count; i++) {
		auto xtx_val = xtx_vec.GetValue(i);
		auto meat_val = meat_vec.GetValue(i);
		int k = k_vec.GetValue(i).GetValue<int32_t>();

		auto &xtx_children = ListValue::GetChildren(xtx_val);
		auto &meat_children = ListValue::GetChildren(meat_val);
		std::vector<double> xtx(static_cast<size_t>(k) * k);
		std::vector<double> meat(static_cast<size_t>(k) * k);
		for (int j = 0; j < k * k; j++) {
			xtx[j] = xtx_children[j].GetValue<double>();
			meat[j] = meat_children[j].GetValue<double>();
		}

		auto diag = dodo::SandwichDiag(xtx, meat, k);

		vector<Value> result_values;
		for (int j = 0; j < k; j++) {
			result_values.push_back(Value::DOUBLE(diag[j]));
		}
		result.SetValue(i, Value::LIST(LogicalType::DOUBLE, std::move(result_values)));
	}
}

static void RegisterOlsFunctions(ExtensionLoader &loader) {
	// dodo_ols_solve(xtx, xty, k) -> DOUBLE[]
	ScalarFunction ols_solve("dodo_ols_solve",
	                         {LogicalType::LIST(LogicalType::DOUBLE), LogicalType::LIST(LogicalType::DOUBLE),
	                          LogicalType::INTEGER},
	                         LogicalType::LIST(LogicalType::DOUBLE), OlsSolveFunction);
	loader.RegisterFunction(ols_solve);

	// dodo_ols_invdiag(xtx, k) -> DOUBLE[]
	ScalarFunction ols_invdiag("dodo_ols_invdiag",
	                           {LogicalType::LIST(LogicalType::DOUBLE), LogicalType::INTEGER},
	                           LogicalType::LIST(LogicalType::DOUBLE), OlsInvDiagFunction);
	loader.RegisterFunction(ols_invdiag);

	// dodo_sandwich_diag(xtx, meat, k) -> DOUBLE[]
	ScalarFunction sandwich_diag("dodo_sandwich_diag",
	                             {LogicalType::LIST(LogicalType::DOUBLE), LogicalType::LIST(LogicalType::DOUBLE),
	                              LogicalType::INTEGER},
	                             LogicalType::LIST(LogicalType::DOUBLE), SandwichDiagFunction);
	loader.RegisterFunction(sandwich_diag);
}

//===--------------------------------------------------------------------===//
// Extension Loading
//===--------------------------------------------------------------------===//

static void LoadInternal(ExtensionLoader &loader) {
	auto &instance = loader.GetDatabaseInstance();
	auto &config = DBConfig::GetConfig(instance);

	RegisterOlsFunctions(loader);

	auto shared_state = make_shared_ptr<DodoStateInfo>();
	g_dodo_state = shared_state.get();

	ParserExtension parser_ext;
	parser_ext.parse_function = dodo_parse;
	parser_ext.plan_function = dodo_plan;
	parser_ext.parser_override = dodo_parser_override;
	parser_ext.parser_info = shared_state;
	ParserExtension::Register(config, parser_ext);

	config.SetOptionByName("allow_parser_override_extension", Value("fallback"));

	auto operator_ext = make_shared_ptr<DodoOperatorExtension>();
	OperatorExtension::Register(config, operator_ext);

	config.AddExtensionOption(
	    "dodo_live_view", "Create/replace _dodo_data view after each transformation (for DuckDB UI)",
	    LogicalType::BOOLEAN, Value::BOOLEAN(false), [](ClientContext &context, SetScope scope, Value &parameter) {
		    if (g_dodo_state) {
			    g_dodo_state->live_view_enabled = parameter.GetValue<bool>();
			    g_dodo_state->core.live_view_enabled = parameter.GetValue<bool>();
		    }
	    });

	config.AddExtensionOption(
	    "dodo_format_sql", "Format generated SQL with indentation and line breaks (default: true)",
	    LogicalType::BOOLEAN, Value::BOOLEAN(true), [](ClientContext &context, SetScope scope, Value &parameter) {
		    if (g_dodo_state) {
			    g_dodo_state->core.format_sql = parameter.GetValue<bool>();
		    }
	    });

	config.AddExtensionOption(
	    "dodo_sql_comments", "Add source command comments to generated SQL CTEs (default: true)",
	    LogicalType::BOOLEAN, Value::BOOLEAN(true), [](ClientContext &context, SetScope scope, Value &parameter) {
		    if (g_dodo_state) {
			    g_dodo_state->core.sql_comments = parameter.GetValue<bool>();
		    }
	    });
}

void DodoExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string DodoExtension::Name() {
	return "dodo";
}

std::string DodoExtension::Version() const {
#ifdef EXT_VERSION_DODO
	return EXT_VERSION_DODO;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(dodo, loader) {
	duckdb::LoadInternal(loader);
}
}
