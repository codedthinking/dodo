#define DUCKDB_EXTENSION_MAIN

#include "dodo_extension.hpp"
#include "regression.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/function/table_function.hpp"
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
// ExecuteReghdfe: parse __REGHDFE__ marker, run data query, demean, OLS
// Returns a VALUES SQL string with the coefficient table.
//===--------------------------------------------------------------------===//
// Execute reghdfe using a new Connection on the given DatabaseInstance.
// For the parser_override path (no ClientContext available).
static string ExecuteReghdfeViaConnection(const string &marker_sql, DodoStateInfo &state, DatabaseInstance &db);

// Execute reghdfe using an existing ClientContext.
// For the dodo_plan path (ClientContext available).
static string ExecuteReghdfeViaContext(const string &marker_sql, DodoStateInfo &state, ClientContext &context);

// Core implementation: parse marker, load data from query result, demean, OLS, return VALUES SQL.
static string ExecuteReghdfeCore(const string &marker_sql, unique_ptr<QueryResult> qresult) {
	string rest = marker_sql.substr(12);
	idx_t params_pos = rest.find("||PARAMS||");
	string params_str = rest.substr(params_pos + 10);

	auto param_lines = StringUtil::Split(params_str, '\n');
	auto var_names_list = StringUtil::Split(param_lines[0], '\t');
	auto fe_names_list = StringUtil::Split(param_lines[1], '\t');
	string cluster_name = param_lines.size() > 2 ? param_lines[2] : "";
	bool is_robust = param_lines.size() > 3 && param_lines[3] == "1";
	if (qresult->HasError()) {
		throw std::runtime_error("reghdfe: " + qresult->GetError());
	}

	int n_vars = static_cast<int>(var_names_list.size());
	int n_fe = static_cast<int>(fe_names_list.size());
	bool has_cluster = !cluster_name.empty();

	std::vector<std::vector<double>> var_cols(n_vars);
	std::vector<std::vector<int>> fe_groups(n_fe);
	std::vector<int> cluster_ids;
	std::vector<std::unordered_map<int64_t, int>> fe_maps(n_fe);
	std::unordered_map<int64_t, int> cluster_map;
	idx_t n_obs = 0;

	unique_ptr<DataChunk> dchunk;
	while ((dchunk = qresult->Fetch()) != nullptr) {
		for (idx_t r = 0; r < dchunk->size(); r++) {
			for (int v = 0; v < n_vars; v++) {
				auto val = dchunk->data[v].GetValue(r);
				var_cols[v].push_back(val.IsNull() ? 0.0 : val.GetValue<double>());
			}
			for (int g = 0; g < n_fe; g++) {
				auto val = dchunk->data[n_vars + g].GetValue(r);
				int64_t raw = val.IsNull() ? -999999 : val.GetValue<int64_t>();
				auto it = fe_maps[g].find(raw);
				int id;
				if (it == fe_maps[g].end()) {
					id = static_cast<int>(fe_maps[g].size());
					fe_maps[g][raw] = id;
				} else {
					id = it->second;
				}
				fe_groups[g].push_back(id);
			}
			if (has_cluster) {
				auto val = dchunk->data[n_vars + n_fe].GetValue(r);
				int64_t raw = val.IsNull() ? -999999 : val.GetValue<int64_t>();
				auto it = cluster_map.find(raw);
				int id;
				if (it == cluster_map.end()) {
					id = static_cast<int>(cluster_map.size());
					cluster_map[raw] = id;
				} else {
					id = it->second;
				}
				cluster_ids.push_back(id);
			}
			n_obs++;
		}
	}

	if (n_obs == 0) {
		return "SELECT 'No observations' AS error";
	}

	std::vector<double> vars_flat(static_cast<size_t>(n_obs) * n_vars);
	for (int v = 0; v < n_vars; v++) {
		for (idx_t i = 0; i < n_obs; i++) {
			vars_flat[static_cast<size_t>(v) * n_obs + i] = var_cols[v][i];
		}
	}
	var_cols.clear();

	std::vector<int> n_fe_levels(n_fe);
	for (int g = 0; g < n_fe; g++) {
		n_fe_levels[g] = static_cast<int>(fe_maps[g].size());
	}

	dodo::MapDemean(vars_flat, static_cast<int>(n_obs), n_vars, fe_groups, n_fe_levels);

	int n = static_cast<int>(n_obs);
	int kx = n_vars - 1;
	std::vector<double> xtx(static_cast<size_t>(kx) * kx, 0.0);
	std::vector<double> xty(kx, 0.0);
	double yty = 0.0;
	double *yp = &vars_flat[0];

	for (int i = 0; i < kx; i++) {
		double *xi = &vars_flat[static_cast<size_t>(i + 1) * n_obs];
		for (idx_t obs = 0; obs < n_obs; obs++) xty[i] += xi[obs] * yp[obs];
		for (int j = 0; j < kx; j++) {
			double *xj = &vars_flat[static_cast<size_t>(j + 1) * n_obs];
			double s = 0.0;
			for (idx_t obs = 0; obs < n_obs; obs++) s += xi[obs] * xj[obs];
			xtx[i * kx + j] = s;
		}
	}
	for (idx_t obs = 0; obs < n_obs; obs++) yty += yp[obs] * yp[obs];

	auto beta = dodo::OlsSolve(xtx, xty, kx);

	std::vector<double> resid(n_obs);
	double ess = 0.0;
	for (idx_t obs = 0; obs < n_obs; obs++) {
		double yhat = 0.0;
		for (int j = 0; j < kx; j++) {
			yhat += beta[j] * vars_flat[static_cast<size_t>(j + 1) * n_obs + obs];
		}
		resid[obs] = yp[obs] - yhat;
		ess += resid[obs] * resid[obs];
	}
	double tss = yty;

	int df_a = 0;
	for (int g = 0; g < n_fe; g++) df_a += n_fe_levels[g];
	if (n_fe >= 2) {
		for (int g = 1; g < n_fe; g++) {
			df_a -= dodo::CountConnectedComponents(fe_groups[0], fe_groups[g],
			                                      n_fe_levels[0], n_fe_levels[g], n);
		}
	} else {
		df_a -= 1;
	}
	int df_r = n - kx - df_a;
	if (df_r <= 0) df_r = 1;

	double s2 = ess / df_r;
	double r2_val = 1.0 - ess / tss;
	double r2_adj = 1.0 - (ess / df_r) / (tss / (n - 1));
	double f_val = ((tss - ess) / kx) / s2;

	std::vector<double> var_diag;
	if (has_cluster) {
		int n_cl = static_cast<int>(cluster_map.size());
		std::vector<double> meat(static_cast<size_t>(kx) * kx, 0.0);
		std::vector<std::vector<double>> cl_scores(n_cl, std::vector<double>(kx, 0.0));
		for (idx_t obs = 0; obs < n_obs; obs++) {
			int cl = cluster_ids[obs];
			for (int j = 0; j < kx; j++)
				cl_scores[cl][j] += resid[obs] * vars_flat[static_cast<size_t>(j + 1) * n_obs + obs];
		}
		for (int cl = 0; cl < n_cl; cl++)
			for (int i = 0; i < kx; i++)
				for (int j = 0; j < kx; j++)
					meat[i * kx + j] += cl_scores[cl][i] * cl_scores[cl][j];
		double qc = ((double)(n - 1) / (n - kx)) * ((double)n_cl / (n_cl - 1));
		for (auto &m : meat) m *= qc;
		var_diag = dodo::SandwichDiag(xtx, meat, kx);
	} else if (is_robust) {
		std::vector<double> meat(static_cast<size_t>(kx) * kx, 0.0);
		for (idx_t obs = 0; obs < n_obs; obs++) {
			double e2 = resid[obs] * resid[obs];
			for (int i = 0; i < kx; i++) {
				double xi = vars_flat[static_cast<size_t>(i + 1) * n_obs + obs];
				for (int j = 0; j < kx; j++)
					meat[i * kx + j] += e2 * xi * vars_flat[static_cast<size_t>(j + 1) * n_obs + obs];
			}
		}
		double hc1 = (double)n / (n - kx);
		for (auto &m : meat) m *= hc1;
		var_diag = dodo::SandwichDiag(xtx, meat, kx);
	} else {
		auto inv_d = dodo::OlsInvDiag(xtx, kx);
		var_diag.resize(kx);
		for (int j = 0; j < kx; j++) var_diag[j] = s2 * inv_d[j];
	}

	string values_sql = "SELECT * FROM (VALUES ";
	for (int j = 0; j < kx; j++) {
		if (j > 0) values_sql += ", ";
		double se_j = std::sqrt(var_diag[j]);
		double t_j = se_j > 0 ? beta[j] / se_j : 0.0;
		double p_j = dodo::NormalPValue(t_j);
		values_sql += "('" + var_names_list[j + 1] + "', " +
		              to_string(beta[j]) + ", " + to_string(se_j) + ", " +
		              to_string(t_j) + ", " + to_string(p_j) + ", " +
		              to_string(beta[j] - 1.96 * se_j) + ", " +
		              to_string(beta[j] + 1.96 * se_j) + ", " +
		              to_string(n) + ", " +
		              to_string(r2_val) + ", " + to_string(r2_adj) + ", " +
		              to_string(f_val) + ")";
	}
	values_sql += ") AS _t(variable, coefficient, std_err, t, p, ci_low, ci_high, N, r2, r2_adj, F)";
	return values_sql;
}

static string ExecuteReghdfeViaConnection(const string &marker_sql, DodoStateInfo &state, DatabaseInstance &db) {
	string rest = marker_sql.substr(12);
	idx_t params_pos = rest.find("||PARAMS||");
	string data_sql = rest.substr(0, params_pos);

	Connection conn(db);
	// Disable parser override on the inner connection to prevent interference
	conn.Query("SET allow_parser_override_extension = 'disabled'");
	auto qresult = conn.Query(data_sql);
	if (qresult->HasError()) {
		throw std::runtime_error("reghdfe: " + qresult->GetError() + "\nSQL: " + data_sql);
	}
	return ExecuteReghdfeCore(marker_sql, std::move(qresult));
}

static string ExecuteReghdfeViaContext(const string &marker_sql, DodoStateInfo &state, ClientContext &context) {
	string rest = marker_sql.substr(12);
	idx_t params_pos = rest.find("||PARAMS||");
	string data_sql = rest.substr(0, params_pos);

	auto qresult = context.Query(data_sql, QueryParameters(false));
	if (qresult->HasError()) {
		throw std::runtime_error("reghdfe: " + qresult->GetError());
	}
	return ExecuteReghdfeCore(marker_sql, std::move(qresult));
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
		// reghdfe — needs parser override for __REGHDFE__ marker handling
		if (StringUtil::StartsWith(trimmed, "reghdfe ")) {
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

				// Handle __REGHDFE__ marker
				if (StringUtil::StartsWith(sql, "__REGHDFE__:")) {
					if (!state.db_instance) {
						throw std::runtime_error("reghdfe: database instance not available");
					}
					// Execute pending SQL (checkpoint) on the inner connection
					// so the data query sees the latest materialized state
					Connection pre_conn(*state.db_instance);
					pre_conn.Query("SET allow_parser_override_extension = 'disabled'");
					for (auto &psql : state.core.pending_sql) {
						pre_conn.Query(psql);
					}
					state.core.pending_sql.clear();

					string values_sql = ExecuteReghdfeViaConnection(sql, state, *state.db_instance);

					Parser vals_parser;
					vals_parser.ParseQuery(values_sql);
					for (auto &st : vals_parser.statements) {
						all_statements.push_back(std::move(st));
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

	// Handle __REGHDFE__ marker: execute data query, run C++ math, return VALUES
	if (StringUtil::StartsWith(sql, "__REGHDFE__:")) {
		// Use a separate connection — this works when dodo._current exists (materialized data)
		// If data is only in the CTE chain (not materialized), the override path handles it instead.
		if (!state.db_instance) {
			throw BinderException("reghdfe: database instance not available");
		}
		string values_sql = ExecuteReghdfeViaConnection(sql, state, *state.db_instance);
		Parser parser;
		parser.ParseQuery(values_sql);
		if (parser.statements.empty()) {
			throw BinderException("reghdfe: generated empty result");
		}
		auto bind_state = make_shared_ptr<DodoBindState>(std::move(parser.statements[0]));
		context.registered_state->Remove("dodo_bind");
		context.registered_state->Insert("dodo_bind", bind_state);
		throw BinderException("dodo redirect to operator bind");
	}

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

//===--------------------------------------------------------------------===//
// reghdfe table function: dodo_reghdfe(query, var_names, fe_names, cluster_name, robust)
// Loads data, demeans w.r.t. FEs, runs OLS, returns coefficient table.
//===--------------------------------------------------------------------===//

struct ReghdfeBindData : public FunctionData {
	string query;
	vector<string> var_names;
	vector<string> fe_names;
	string cluster_name;
	bool robust = false;

	// Results computed in bind
	vector<string> result_names;
	vector<double> coefs;
	vector<double> se;
	vector<double> t_stat;
	vector<double> p_val;
	vector<double> ci_low;
	vector<double> ci_high;
	int n_obs = 0;
	double r2 = 0;
	double r2_adj = 0;
	double f_stat = 0;

	unique_ptr<FunctionData> Copy() const override {
		auto result = make_uniq<ReghdfeBindData>();
		*result = *this;
		return std::move(result);
	}
	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<ReghdfeBindData>();
		return query == other.query && var_names == other.var_names &&
		       fe_names == other.fe_names && cluster_name == other.cluster_name;
	}
};

struct ReghdfeGlobalState : public GlobalTableFunctionState {
	bool done = false;
};

static unique_ptr<FunctionData> ReghdfeBind(ClientContext &context, TableFunctionBindInput &input,
                                            vector<LogicalType> &return_types, vector<string> &names) {
	auto bind_data = make_uniq<ReghdfeBindData>();
	bind_data->query = input.inputs[0].ToString();

	auto &var_list = ListValue::GetChildren(input.inputs[1]);
	for (auto &v : var_list) {
		bind_data->var_names.push_back(v.ToString());
	}
	auto &fe_list = ListValue::GetChildren(input.inputs[2]);
	for (auto &f : fe_list) {
		bind_data->fe_names.push_back(f.ToString());
	}
	bind_data->cluster_name = input.inputs[3].ToString();
	bind_data->robust = input.inputs[4].GetValue<bool>();

	// Output schema: coefficient table
	return_types = {LogicalType::VARCHAR, LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE,
	                LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE,
	                LogicalType::INTEGER, LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE};
	names = {"variable", "coefficient", "std_err", "t", "p", "ci_low", "ci_high",
	         "N", "r2", "r2_adj", "F"};

	// Do all computation in bind (we have context access here without deadlock)
	// Read from the pre-materialized temp table
	auto result = context.Query("SELECT * FROM " + bind_data->query, QueryParameters(false));
	if (result->HasError()) {
		throw InvalidInputException("reghdfe: failed to execute data query: %s", result->GetError());
	}

	int n_vars = static_cast<int>(bind_data->var_names.size());
	int n_fe = static_cast<int>(bind_data->fe_names.size());
	bool has_cluster = !bind_data->cluster_name.empty();

	// First pass: count rows and collect data
	// We use Fetch() to iterate chunks
	std::vector<std::vector<double>> var_cols(n_vars); // one vector per variable
	std::vector<std::vector<int>> fe_groups(n_fe);
	std::vector<int> cluster_ids;
	std::vector<std::unordered_map<int64_t, int>> fe_maps(n_fe);
	std::unordered_map<int64_t, int> cluster_map;

	idx_t n_obs = 0;
	unique_ptr<DataChunk> chunk;
	while ((chunk = result->Fetch()) != nullptr) {
		for (idx_t r = 0; r < chunk->size(); r++) {
			for (int v = 0; v < n_vars; v++) {
				auto val = chunk->data[v].GetValue(r);
				if (val.IsNull()) {
					throw InvalidInputException("reghdfe: NULL values not supported (variable '%s', row %d)",
					                           bind_data->var_names[v], (int)n_obs);
				}
				var_cols[v].push_back(val.GetValue<double>());
			}
			for (int g = 0; g < n_fe; g++) {
				auto val = chunk->data[n_vars + g].GetValue(r);
				int64_t raw = val.IsNull() ? -999999 : val.GetValue<int64_t>();
				auto it = fe_maps[g].find(raw);
				int id;
				if (it == fe_maps[g].end()) {
					id = static_cast<int>(fe_maps[g].size());
					fe_maps[g][raw] = id;
				} else {
					id = it->second;
				}
				fe_groups[g].push_back(id);
			}
			if (has_cluster) {
				auto val = chunk->data[n_vars + n_fe].GetValue(r);
				int64_t raw = val.IsNull() ? -999999 : val.GetValue<int64_t>();
				auto it = cluster_map.find(raw);
				int id;
				if (it == cluster_map.end()) {
					id = static_cast<int>(cluster_map.size());
					cluster_map[raw] = id;
				} else {
					id = it->second;
				}
				cluster_ids.push_back(id);
			}
			n_obs++;
		}
	}

	if (n_obs == 0) {
		return std::move(bind_data);
	}

	// Convert to column-major flat array for MapDemean
	std::vector<double> vars(static_cast<size_t>(n_obs) * n_vars);
	for (int v = 0; v < n_vars; v++) {
		for (idx_t i = 0; i < n_obs; i++) {
			vars[static_cast<size_t>(v) * n_obs + i] = var_cols[v][i];
		}
	}
	var_cols.clear(); // free memory

	// Number of FE levels
	std::vector<int> n_fe_levels(n_fe);
	for (int g = 0; g < n_fe; g++) {
		n_fe_levels[g] = static_cast<int>(fe_maps[g].size());
	}

	// Demean all variables (y and x's) w.r.t. FE dimensions
	dodo::MapDemean(vars, static_cast<int>(n_obs), n_vars, fe_groups, n_fe_levels);

	// OLS on demeaned data (no intercept)
	// Build X'X and X'y from demeaned data
	// x columns are vars[1..k-1], y is vars[0]
	int n = static_cast<int>(n_obs);
	int kx = n_vars - 1; // number of x variables (excluding y)
	if (kx == 0) {
		throw InvalidInputException("reghdfe: need at least one independent variable");
	}

	std::vector<double> xtx(static_cast<size_t>(kx) * kx, 0.0);
	std::vector<double> xty(kx, 0.0);
	double yty = 0.0;

	double *y = &vars[0]; // first column is y
	for (int i = 0; i < kx; i++) {
		double *xi = &vars[static_cast<size_t>(i + 1) * n_obs];
		for (idx_t obs = 0; obs < n_obs; obs++) {
			xty[i] += xi[obs] * y[obs];
		}
		for (int j = 0; j < kx; j++) {
			double *xj = &vars[static_cast<size_t>(j + 1) * n_obs];
			double s = 0.0;
			for (idx_t obs = 0; obs < n_obs; obs++) {
				s += xi[obs] * xj[obs];
			}
			xtx[i * kx + j] = s;
		}
	}
	for (idx_t obs = 0; obs < n_obs; obs++) {
		yty += y[obs] * y[obs];
	}

	auto beta = dodo::OlsSolve(xtx, xty, kx);

	// Residuals and fit stats
	std::vector<double> resid(n_obs);
	double ess = 0.0;
	for (idx_t obs = 0; obs < n_obs; obs++) {
		double yhat = 0.0;
		for (int j = 0; j < kx; j++) {
			yhat += beta[j] * vars[static_cast<size_t>(j + 1) * n_obs + obs];
		}
		resid[obs] = y[obs] - yhat;
		ess += resid[obs] * resid[obs];
	}
	double tss = yty; // demeaned y, so TSS = y'y

	// Degrees of freedom
	int df_a = 0;
	for (int g = 0; g < n_fe; g++) {
		df_a += n_fe_levels[g];
	}
	// Subtract redundant parameters (connected components for pairs of FEs)
	if (n_fe >= 2) {
		for (int g = 1; g < n_fe; g++) {
			int comps = dodo::CountConnectedComponents(fe_groups[0], fe_groups[g],
			                                          n_fe_levels[0], n_fe_levels[g], n);
			df_a -= comps;
		}
	} else {
		df_a -= 1; // intercept is redundant with a single FE
	}

	int df_r = n - kx - df_a;
	if (df_r <= 0) {
		throw InvalidInputException("reghdfe: insufficient degrees of freedom (N=%d, k=%d, df_a=%d)", n, kx, df_a);
	}

	double s2 = ess / df_r;
	double r2 = 1.0 - ess / tss;
	double r2_adj = 1.0 - (ess / df_r) / (tss / (n - 1));
	double f_stat = ((tss - ess) / kx) / s2;

	// Standard errors
	std::vector<double> var_diag;
	bool use_cluster = !bind_data->cluster_name.empty();

	if (use_cluster) {
		// Cluster-robust SE
		int n_clusters = static_cast<int>(cluster_map.size());
		// Meat: sum of (sum_g e_i * x_i)(sum_g e_i * x_i)' over clusters
		std::vector<double> meat(static_cast<size_t>(kx) * kx, 0.0);
		// Accumulate score vectors per cluster
		std::vector<std::vector<double>> cluster_scores(n_clusters, std::vector<double>(kx, 0.0));
		for (idx_t obs = 0; obs < n_obs; obs++) {
			int cl = cluster_ids[obs];
			for (int j = 0; j < kx; j++) {
				cluster_scores[cl][j] += resid[obs] * vars[static_cast<size_t>(j + 1) * n_obs + obs];
			}
		}
		for (int cl = 0; cl < n_clusters; cl++) {
			for (int i = 0; i < kx; i++) {
				for (int j = 0; j < kx; j++) {
					meat[i * kx + j] += cluster_scores[cl][i] * cluster_scores[cl][j];
				}
			}
		}
		// Finite-sample correction: (N-1)/(N-k) * M/(M-1)
		double qc = ((double)(n - 1) / (n - kx)) * ((double)n_clusters / (n_clusters - 1));
		for (auto &m : meat) {
			m *= qc;
		}
		var_diag = dodo::SandwichDiag(xtx, meat, kx);
	} else if (bind_data->robust) {
		// HC1 robust SE
		std::vector<double> meat(static_cast<size_t>(kx) * kx, 0.0);
		for (idx_t obs = 0; obs < n_obs; obs++) {
			double e2 = resid[obs] * resid[obs];
			for (int i = 0; i < kx; i++) {
				double xi = vars[static_cast<size_t>(i + 1) * n_obs + obs];
				for (int j = 0; j < kx; j++) {
					double xj = vars[static_cast<size_t>(j + 1) * n_obs + obs];
					meat[i * kx + j] += e2 * xi * xj;
				}
			}
		}
		double hc1 = (double)n / (n - kx);
		for (auto &m : meat) {
			m *= hc1;
		}
		var_diag = dodo::SandwichDiag(xtx, meat, kx);
	} else {
		// Conventional SE
		auto inv_diag = dodo::OlsInvDiag(xtx, kx);
		var_diag.resize(kx);
		for (int j = 0; j < kx; j++) {
			var_diag[j] = s2 * inv_diag[j];
		}
	}

	// Build result
	for (int j = 0; j < kx; j++) {
		bind_data->result_names.push_back(bind_data->var_names[j + 1]); // skip depvar
		bind_data->coefs.push_back(beta[j]);
		double se_j = std::sqrt(var_diag[j]);
		bind_data->se.push_back(se_j);
		double t_j = se_j > 0 ? beta[j] / se_j : 0.0;
		bind_data->t_stat.push_back(t_j);
		bind_data->p_val.push_back(dodo::NormalPValue(t_j));
		bind_data->ci_low.push_back(beta[j] - 1.96 * se_j);
		bind_data->ci_high.push_back(beta[j] + 1.96 * se_j);
	}
	bind_data->n_obs = n;
	bind_data->r2 = r2;
	bind_data->r2_adj = r2_adj;
	bind_data->f_stat = f_stat;

	return std::move(bind_data);
}

static unique_ptr<GlobalTableFunctionState> ReghdfeInit(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<ReghdfeGlobalState>();
}

static void ReghdfeExecute(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &gstate = data.global_state->Cast<ReghdfeGlobalState>();
	if (gstate.done) {
		output.SetCardinality(0);
		return;
	}
	gstate.done = true;

	auto &bd = data.bind_data->Cast<ReghdfeBindData>();
	idx_t n_rows = bd.result_names.size();
	if (n_rows == 0) {
		output.SetCardinality(0);
		return;
	}

	output.SetCardinality(n_rows);
	for (idx_t i = 0; i < n_rows; i++) {
		output.data[0].SetValue(i, Value(bd.result_names[i]));
		output.data[1].SetValue(i, Value::DOUBLE(bd.coefs[i]));
		output.data[2].SetValue(i, Value::DOUBLE(bd.se[i]));
		output.data[3].SetValue(i, Value::DOUBLE(bd.t_stat[i]));
		output.data[4].SetValue(i, Value::DOUBLE(bd.p_val[i]));
		output.data[5].SetValue(i, Value::DOUBLE(bd.ci_low[i]));
		output.data[6].SetValue(i, Value::DOUBLE(bd.ci_high[i]));
		output.data[7].SetValue(i, Value::INTEGER(bd.n_obs));
		output.data[8].SetValue(i, Value::DOUBLE(bd.r2));
		output.data[9].SetValue(i, Value::DOUBLE(bd.r2_adj));
		output.data[10].SetValue(i, Value::DOUBLE(bd.f_stat));
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

	// dodo_reghdfe(query, var_names, fe_names, cluster_name, robust) -> coefficient table
	TableFunction reghdfe_func("dodo_reghdfe",
	                           {LogicalType::VARCHAR,
	                            LogicalType::LIST(LogicalType::VARCHAR),
	                            LogicalType::LIST(LogicalType::VARCHAR),
	                            LogicalType::VARCHAR,
	                            LogicalType::BOOLEAN},
	                           ReghdfeExecute, ReghdfeBind, ReghdfeInit);
	loader.RegisterFunction(reghdfe_func);
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
	shared_state->db_instance = &instance;

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
