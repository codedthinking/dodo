#include "dodo_core.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

static const char *DODOC_VERSION = "0.3.0";

static void print_usage(const char *prog) {
	std::cerr << "Usage: " << prog << " [OPTIONS] [INPUT_FILE]\n"
	          << "\n"
	          << "Compile .do files to SQL.\n"
	          << "\n"
	          << "Arguments:\n"
	          << "  INPUT_FILE           .do file to compile (default: stdin)\n"
	          << "\n"
	          << "Options:\n"
	          << "  -o, --output FILE    Write SQL to FILE (default: stdout)\n"
	          << "  --annotate           Emit original .do command as SQL comment\n"
	          << "  --no-terminal        Suppress SQL for terminal commands (count, describe, etc.)\n"
	          << "  --terminal           Emit SQL for terminal commands (default)\n"
	          << "  --ast                Emit lineage AST as JSON (one node per line)\n"
	          << "  -v, --version        Show version\n"
	          << "  -h, --help           Show this help message\n";
}

struct CliOptions {
	std::string input_file;  // empty = stdin
	std::string output_file; // empty = stdout
	bool annotate = false;
	bool terminal = true;
	bool ast = false;
};

static CliOptions parse_args(int argc, char *argv[]) {
	CliOptions opts;
	for (int i = 1; i < argc; i++) {
		std::string arg = argv[i];
		if (arg == "-h" || arg == "--help") {
			print_usage(argv[0]);
			std::exit(0);
		} else if (arg == "-v" || arg == "--version") {
			std::cout << "dodoc " << DODOC_VERSION << "\n";
			std::exit(0);
		} else if (arg == "-o" || arg == "--output") {
			if (i + 1 >= argc) {
				std::cerr << "Error: " << arg << " requires a filename\n";
				std::exit(1);
			}
			opts.output_file = argv[++i];
		} else if (arg == "--annotate") {
			opts.annotate = true;
		} else if (arg == "--terminal") {
			opts.terminal = true;
		} else if (arg == "--no-terminal") {
			opts.terminal = false;
		} else if (arg == "--ast") {
			opts.ast = true;
		} else if (arg[0] == '-') {
			std::cerr << "Unknown option: " << arg << "\n";
			print_usage(argv[0]);
			std::exit(1);
		} else {
			opts.input_file = arg;
		}
	}
	return opts;
}

// Process lines from an input stream using shared ProcessLines engine
static void process_stream(std::istream &input, dodo::DodoState &state, std::vector<std::string> &side_effect_sql,
                           const CliOptions &opts) {
	dodo::LineReader reader = [&](std::string &out) -> bool {
		return !!std::getline(input, out);
	};
	auto results = dodo::ProcessLines(reader, state, /*skip_terminal=*/!opts.terminal);
	for (auto &sql : results) {
		side_effect_sql.push_back(sql);
	}
}

// JSON serialization helpers for --ast output
static std::string verb_str(dodo::Verb v) {
	switch (v) {
	case dodo::Verb::LOAD: return "load";
	case dodo::Verb::MERGE: return "merge";
	case dodo::Verb::ASSIGN: return "assign";
	case dodo::Verb::SCALAR: return "scalar";
	case dodo::Verb::FILTER: return "filter";
	case dodo::Verb::REORDER: return "reorder";
	case dodo::Verb::LAG: return "lag";
	case dodo::Verb::BYOP: return "byop";
	case dodo::Verb::EMIT: return "emit";
	}
	return "unknown";
}

static std::string cert_str(dodo::Certainty c) {
	switch (c) {
	case dodo::Certainty::RESOLVED: return "resolved";
	case dodo::Certainty::ABBREV: return "abbrev";
	case dodo::Certainty::UNKNOWN: return "unknown";
	}
	return "unknown";
}

static std::string json_escape(const std::string &s) {
	std::string out;
	out.reserve(s.size() + 8);
	for (char c : s) {
		switch (c) {
		case '"': out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default: out += c;
		}
	}
	return out;
}

static std::string json_str_array(const std::vector<std::string> &arr) {
	std::string out = "[";
	for (size_t i = 0; i < arr.size(); i++) {
		if (i > 0) out += ", ";
		out += "\"" + json_escape(arr[i]) + "\"";
	}
	out += "]";
	return out;
}

static void emit_ast_json(std::ostream &out, const dodo::DodoState &state) {
	for (auto &node : state.lineage) {
		out << "{\"verb\": \"" << verb_str(node.verb) << "\"";
		out << ", \"loc\": {\"script\": \"" << json_escape(node.loc.script) << "\", \"line\": " << node.loc.line << "}";
		out << ", \"cert\": \"" << cert_str(node.cert) << "\"";
		out << ", \"targets\": " << json_str_array(node.targets);
		out << ", \"sources\": " << json_str_array(node.sources);
		if (!node.rowset_in.empty()) out << ", \"rowset_in\": \"" << node.rowset_in << "\"";
		if (!node.rowset_out.empty()) out << ", \"rowset_out\": \"" << node.rowset_out << "\"";
		out << ", \"expression\": \"" << json_escape(node.expression) << "\"";
		out << ", \"sql_ref\": \"" << node.sql_ref << "\"";
		if (!node.payload.empty()) {
			out << ", \"payload\": {";
			bool first = true;
			for (auto &[k, v] : node.payload) {
				if (!first) out << ", ";
				out << "\"" << json_escape(k) << "\": \"" << json_escape(v) << "\"";
				first = false;
			}
			out << "}";
		}
		if (!node.warnings.empty()) out << ", \"warnings\": " << json_str_array(node.warnings);
		out << "}\n";
	}
}

int main(int argc, char *argv[]) {
	auto opts = parse_args(argc, argv);

	dodo::DodoState state;
	std::vector<std::string> side_effect_sql;

	// Set script name from input file
	if (!opts.input_file.empty()) {
		auto slash = opts.input_file.rfind('/');
		auto dot = opts.input_file.rfind('.');
		size_t start = (slash == std::string::npos) ? 0 : slash + 1;
		if (dot == std::string::npos || dot < start) dot = opts.input_file.size();
		state.current_script = opts.input_file.substr(start, dot - start);
	} else {
		state.current_script = "<stdin>";
	}

	try {
		if (opts.input_file.empty()) {
			// Read from stdin
			process_stream(std::cin, state, side_effect_sql, opts);
		} else {
			std::ifstream input(opts.input_file);
			if (!input.is_open()) {
				std::cerr << "Error: cannot open file: " << opts.input_file << "\n";
				return 1;
			}
			process_stream(input, state, side_effect_sql, opts);
		}
	} catch (const dodo::DodoException &ex) {
		std::cerr << "Error: " << ex.what() << "\n";
		return 1;
	}

	// Open output
	std::ostream *out = &std::cout;
	std::ofstream out_file;
	if (!opts.output_file.empty()) {
		out_file.open(opts.output_file);
		if (!out_file.is_open()) {
			std::cerr << "Error: cannot open output file: " << opts.output_file << "\n";
			return 1;
		}
		out = &out_file;
	}

	// AST mode: emit lineage JSON instead of SQL
	if (opts.ast) {
		emit_ast_json(*out, state);
		return 0;
	}

	// Emit side-effect SQL (CREATE TABLE, SET VARIABLE, COPY TO, etc.)
	bool has_terminal_side_effect = false;
	for (auto &sql : side_effect_sql) {
		// Skip "SELECT 'OK' AS status" lines
		if (sql.find("SELECT 'OK' AS status") != std::string::npos) {
			continue;
		}
		*out << sql << ";\n";
		// COPY TO embeds the full CTE — don't also emit standalone CTE
		if (sql.find("COPY (") != std::string::npos) {
			has_terminal_side_effect = true;
		}
	}

	// Emit the final CTE chain query if there is data and no terminal
	// side-effect already consumed it (save/export embed the full CTE in COPY TO)
	if (state.HasData() && !has_terminal_side_effect) {
		if (opts.annotate) {
			for (size_t i = 0; i < state.cte_commands.size(); i++) {
				*out << "-- " << state.cte_commands[i] << "\n";
			}
		}
		*out << state.BuildQuery("SELECT * FROM " + state.LatestStep()) << ";\n";
	}

	return 0;
}
