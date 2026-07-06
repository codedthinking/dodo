#include "do_lexer.hpp"

#include <cctype>

namespace dodo {
namespace lex {

using std::size_t;
using std::string;
using std::vector;

string StripComments(const string &line, bool &in_block_comment, bool &line_continued) {
	line_continued = false;
	string out;
	size_t n = line.size();
	size_t i = 0;

	// Resume an open block comment: drop everything up to a closing */.
	if (in_block_comment) {
		size_t end = line.find("*/");
		if (end == string::npos) {
			return ""; // whole line still inside the block comment
		}
		in_block_comment = false;
		i = end + 2;
	} else {
		// A '*' as the first non-blank character makes the whole line a comment.
		// (Only at true line start — never after a resumed block comment, where a
		// trailing '*' would be multiplication.)
		size_t j = i;
		while (j < n && (line[j] == ' ' || line[j] == '\t')) {
			j++;
		}
		if (j < n && line[j] == '*') {
			return "";
		}
	}

	bool in_string = false;
	while (i < n) {
		char c = line[i];

		if (in_string) {
			out += c;
			if (c == '"') {
				in_string = false;
			}
			i++;
			continue;
		}

		if (c == '"') {
			in_string = true;
			out += c;
			i++;
			continue;
		}

		// Block comment /* ... */
		if (c == '/' && i + 1 < n && line[i + 1] == '*') {
			size_t end = line.find("*/", i + 2);
			if (end == string::npos) {
				in_block_comment = true;
				break; // rest of line is inside the block comment
			}
			i = end + 2;
			continue;
		}

		// Line comment // (Stata: must be preceded by whitespace or line start).
		if (c == '/' && i + 1 < n && line[i + 1] == '/') {
			bool preceded_ok = (i == 0 || line[i - 1] == ' ' || line[i - 1] == '\t');
			if (preceded_ok) {
				if (i + 2 < n && line[i + 2] == '/') {
					line_continued = true; // /// continuation
				}
				break; // drop the rest of the line
			}
		}

		out += c;
		i++;
	}

	return out;
}

vector<string> SplitOutsideQuotes(const string &s, char delim) {
	vector<string> result;
	string current;
	char quote = 0; // 0 = not in a string; otherwise the opening quote char
	int paren_depth = 0;
	for (char c : s) {
		if (quote) {
			current += c;
			if (c == quote) {
				quote = 0;
			}
			continue;
		}
		if (c == '"' || c == '\'') {
			quote = c;
			current += c;
			continue;
		}
		if (c == '(') {
			paren_depth++;
			current += c;
			continue;
		}
		if (c == ')') {
			if (paren_depth > 0) {
				paren_depth--;
			}
			current += c;
			continue;
		}
		if (c == delim && paren_depth == 0) {
			result.push_back(current);
			current.clear();
			continue;
		}
		current += c;
	}
	result.push_back(current);
	return result;
}

size_t FindKeywordOutsideQuotes(const string &s, const string &kw) {
	if (kw.empty() || kw.size() > s.size()) {
		return string::npos;
	}
	char quote = 0;
	int paren_depth = 0;
	for (size_t i = 0; i + kw.size() <= s.size(); i++) {
		char c = s[i];
		if (quote) {
			if (c == quote) {
				quote = 0;
			}
			continue;
		}
		if (c == '"' || c == '\'') {
			quote = c;
			continue;
		}
		if (c == '(') {
			paren_depth++;
			continue;
		}
		if (c == ')') {
			if (paren_depth > 0) {
				paren_depth--;
			}
			continue;
		}
		if (paren_depth == 0 && s.compare(i, kw.size(), kw) == 0) {
			return i;
		}
	}
	return string::npos;
}

vector<Token> Tokenize(const string &s) {
	vector<Token> tokens;
	size_t i = 0;
	size_t n = s.size();
	while (i < n) {
		while (i < n && (s[i] == ' ' || s[i] == '\t')) {
			i++;
		}
		if (i >= n) {
			break;
		}
		if (s[i] == '"') {
			// Quoted string: content up to the next unescaped closing quote.
			size_t start = i + 1;
			size_t end = s.find('"', start);
			if (end == string::npos) {
				end = n;
			}
			tokens.push_back({Token::STRING, s.substr(start, end - start)});
			i = (end < n) ? end + 1 : end;
		} else {
			size_t start = i;
			while (i < n && s[i] != ' ' && s[i] != '\t' && s[i] != '"') {
				i++;
			}
			tokens.push_back({Token::WORD, s.substr(start, i - start)});
		}
	}
	return tokens;
}

int BraceDelta(const string &line) {
	int delta = 0;
	bool in_string = false;
	for (char c : line) {
		if (in_string) {
			if (c == '"') {
				in_string = false;
			}
			continue;
		}
		if (c == '"') {
			in_string = true;
		} else if (c == '{') {
			delta++;
		} else if (c == '}') {
			delta--;
		}
	}
	return delta;
}

} // namespace lex
} // namespace dodo
