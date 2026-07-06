#pragma once

#include "string_utils.hpp"

#include <string>
#include <vector>

namespace dodo {
namespace lex {

//===--------------------------------------------------------------------===//
// do_lexer — one quote/comment/brace-aware scanner for .do source lines.
//
// Every place that needs to understand .do lexical structure (comment
// stripping, statement splitting, `if`/`,`/`:` detection, brace accumulation,
// token splitting) routes through here, so quote-awareness is implemented once
// instead of re-derived (or forgotten) at each call site.
//
// String handling: Stata double-quoted strings "..." are recognized. Compound
// double quotes `"..."' are handled best-effort (the inner `"` toggles string
// state); fully nested compound strings are a known limitation.
//===--------------------------------------------------------------------===//

// Strip Stata comments from a single physical line, returning the code portion.
//   * whole-line comment when the first non-blank char is '*'
//   // line comment when preceded by whitespace/line-start and not inside a string
//   /// line continuation (same rule as //), reported via `line_continued`
//   /* ... */ block comment, possibly spanning lines via `in_block_comment`
// `in_block_comment` is read and updated (true when a block comment is still
// open at end of line). `line_continued` is set true when the line ended with a
// /// continuation. Unlike a naive find("//"), a `//` not preceded by whitespace
// (e.g. inside "http://x") is NOT a comment.
std::string StripComments(const std::string &line, bool &in_block_comment, bool &line_continued);

// Split `s` on `delim`, ignoring delimiters inside "..." strings and (...) groups.
std::vector<std::string> SplitOutsideQuotes(const std::string &s, char delim);

// Find the first occurrence of `kw` that lies outside "..." strings and (...)
// groups. Returns std::string::npos if not found.
std::size_t FindKeywordOutsideQuotes(const std::string &s, const std::string &kw);

// Tokenize into whitespace-separated words and quoted strings. String tokens
// carry their inner (unquoted) content and kind == STRING.
struct Token {
	enum Kind { WORD, STRING };
	Kind kind;
	std::string text;
};
std::vector<Token> Tokenize(const std::string &s);

// Net brace depth change of a line: (# of '{') - (# of '}'), ignoring braces
// inside "..." strings.
int BraceDelta(const std::string &line);

} // namespace lex
} // namespace dodo
