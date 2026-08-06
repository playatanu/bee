#pragma once
#include "token.hpp"
#include <vector>
#include <string>
#include <stdexcept>

namespace bee {

struct LexError : std::runtime_error {
    int line;
    std::string message;   // see ParseError::message
    LexError(const std::string& msg, int ln)
        : std::runtime_error("Lex error (line " + std::to_string(ln) + "): " + msg),
          line(ln), message(msg) {}
};

// Decode the escape sequences of a string body (\n, \t, \\, quotes, \0).
// Shared with the parser, which decodes the literal chunks of an interpolated
// string after splitting it on its {expressions}.
std::string decodeStringEscapes(const std::string& raw);

class Lexer {
public:
    explicit Lexer(std::string source) : src(std::move(source)) {}
    std::vector<Token> tokenize();

private:
    std::string src;
    size_t pos = 0;
    int line = 1;

    bool atEnd() const { return pos >= src.size(); }
    char peek() const { return atEnd() ? '\0' : src[pos]; }
    char peekNext() const { return (pos + 1 >= src.size()) ? '\0' : src[pos + 1]; }
    char advance() { return src[pos++]; }
    bool match(char expected);

    void addString(std::vector<Token>& out, char quote);
    // f"..." -- captured raw (escapes and braces intact) for the parser to split.
    void addInterpString(std::vector<Token>& out, char quote);
    void addNumber(std::vector<Token>& out);
    void addIdentifier(std::vector<Token>& out);
};

} // namespace bee
