#pragma once
#include "token.hpp"
#include <vector>
#include <string>
#include <stdexcept>

namespace bee {

struct LexError : std::runtime_error {
    int line;
    LexError(const std::string& msg, int ln)
        : std::runtime_error("Lex error (line " + std::to_string(ln) + "): " + msg), line(ln) {}
};

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
    void addNumber(std::vector<Token>& out);
    void addIdentifier(std::vector<Token>& out);
};

} // namespace bee
