// Unit tests for the do_lexer module. Built and run by `make test-core`.
#include "do_lexer.hpp"

#include <cstdio>
#include <string>
#include <vector>

using namespace dodo::lex;

static int failures = 0;

static void check(bool cond, const std::string &what) {
	if (!cond) {
		std::printf("  FAIL: %s\n", what.c_str());
		failures++;
	}
}

static std::string strip(const std::string &line) {
	bool blk = false, cont = false;
	return StripComments(line, blk, cont);
}

int main() {
	// --- StripComments ---
	check(strip("generate x = 1") == "generate x = 1", "no-comment passthrough");
	// // inside a string / URL is NOT a comment (not preceded by whitespace)
	check(strip("generate url = \"http://example.com\"") == "generate url = \"http://example.com\"",
	      "// in URL not stripped");
	// // preceded by whitespace IS a comment
	check(strip("generate x = 1 // set x") == "generate x = 1 ", "// comment stripped");
	// // preceded by whitespace but inside a string is NOT a comment
	check(strip("generate s = \"a // b\"") == "generate s = \"a // b\"", "// inside string kept");
	// leading * is a whole-line comment
	check(strip("* a comment") == "", "leading * comment");
	check(strip("   * indented comment") == "", "indented * comment");
	// * as multiplication is not a comment
	check(strip("generate y = a * b") == "generate y = a * b", "* multiplication kept");
	// inline block comment
	check(strip("generate x = /* c */ 1") == "generate x =  1", "inline block comment");

	// block comment spanning lines
	{
		bool blk = false, cont = false;
		std::string a = StripComments("generate x = 1 /* open", blk, cont);
		check(a == "generate x = 1 " && blk, "block comment opens");
		std::string b = StripComments("still comment", blk, cont);
		check(b == "" && blk, "block comment continues");
		std::string c = StripComments("close */ generate y = 2", blk, cont);
		check(c == " generate y = 2" && !blk, "block comment closes");
	}
	// /// continuation
	{
		bool blk = false, cont = false;
		std::string a = StripComments("generate x = 1 + /// keep going", blk, cont);
		check(a == "generate x = 1 + " && cont, "/// continuation flagged");
	}

	// --- SplitOutsideQuotes ---
	{
		auto parts = SplitOutsideQuotes("a, b, c", ',');
		check(parts.size() == 3 && parts[0] == "a" && parts[1] == " b", "split on comma");
		auto q = SplitOutsideQuotes("keep if x == \"a,b\", opts", ',');
		check(q.size() == 2, "comma inside string ignored");
		auto p = SplitOutsideQuotes("f(a, b), c", ',');
		check(p.size() == 2, "comma inside parens ignored");
		auto semi = SplitOutsideQuotes("SELECT ';' AS x; SELECT 2", ';');
		check(semi.size() == 2, "semicolon inside string ignored (SQL mode)");
		// .do-mode ("\"" only): an apostrophe is not a string delimiter, so it
		// must not swallow a later comma.
		auto apo = SplitOutsideQuotes("label don't stop, replace", ',', "\"");
		check(apo.size() == 2, ".do-mode: apostrophe does not open a string");
		// but a double-quoted string still protects its comma in .do-mode
		auto dq = SplitOutsideQuotes("keep if x == \"a,b\", opts", ',', "\"");
		check(dq.size() == 2, ".do-mode: comma in double-quoted string ignored");
	}

	// --- FindKeywordOutsideQuotes ---
	{
		check(FindKeywordOutsideQuotes("keep price if x > 0", " if ") == 10, "find ' if '");
		check(FindKeywordOutsideQuotes("keep if name == \" if \"", " if ") == 4, "first ' if ' outside string");
		check(FindKeywordOutsideQuotes("gen s = \" if \"", " if ") == std::string::npos, "' if ' only in string");
		// .do-mode: an apostrophe (e.g. from "don't") must not hide a real ' if '
		check(FindKeywordOutsideQuotes("keep don't if x > 0", " if ", "\"") == 10,
		      ".do-mode: apostrophe does not hide ' if '");
	}

	// --- Tokenize ---
	{
		auto t = Tokenize("a b c");
		check(t.size() == 3 && t[0].text == "a", "tokenize words");
		auto q = Tokenize("a \"b c\" d");
		check(q.size() == 3 && q[1].kind == Token::STRING && q[1].text == "b c", "tokenize quoted");
	}

	// --- BraceDelta ---
	check(BraceDelta("foreach x in a b {") == 1, "open brace");
	check(BraceDelta("}") == -1, "close brace");
	check(BraceDelta("generate s = \"{not a brace}\"") == 0, "braces in string ignored");
	check(BraceDelta("if { nested {") == 2, "two opens");

	if (failures == 0) {
		std::printf("lexer: all unit tests passed\n");
		return 0;
	}
	std::printf("lexer: %d unit test(s) FAILED\n", failures);
	return 1;
}
