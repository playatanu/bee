#pragma once
#include "token.hpp"
#include "ast.hpp"
#include <vector>
#include <stdexcept>
#include <string>

namespace bee {

struct ParseError : std::runtime_error {
    int line;
    std::string message;   // without the "Parse error (line N):" prefix, so the
                           // caller can re-render it with the file name
    // True when the parser ran out of input rather than finding something wrong:
    // the REPL treats that as "keep typing" instead of an error.
    bool atEnd = false;
    ParseError(const std::string& msg, int ln, bool end = false)
        : std::runtime_error("Parse error (line " + std::to_string(ln) + "): " + msg),
          line(ln), message(msg), atEnd(end) {}
};

class Parser {
public:
    explicit Parser(std::vector<Token> toks) : tokens(std::move(toks)) {}
    Program parse();

private:
    // f"a={a}" desugars to ("a=" + (a)), reusing the interpreter's rule that a
    // concatenation with a string stringifies the other side.
    ExprPtr interpolatedString(const Token& tok);

    std::vector<Token> tokens;
    size_t current = 0;

    // --- token helpers ---
    const Token& peek() const { return tokens[current]; }
    const Token& previous() const { return tokens[current - 1]; }
    bool atEnd() const { return peek().type == TokenType::EOF_TOK; }
    const Token& advance() { if (!atEnd()) current++; return previous(); }
    bool check(TokenType t) const { return !atEnd() && peek().type == t; }
    bool match(TokenType t) { if (check(t)) { advance(); return true; } return false; }
    bool matchAny(std::initializer_list<TokenType> types);
    const Token& consume(TokenType t, const std::string& msg);

    // --- statements ---
    StmtPtr declaration();
    StmtPtr letStatement(bool isConst = false);
    void parseParams(FunctionStmt* fn);
    TypeAnn typeAnnotation();   // after the ':' (or legacy '->') that introduces it
    std::unique_ptr<FunctionStmt> functionDecl(const std::string& kind);
    StmtPtr classDeclaration();
    StmtPtr importStatement();
    StmtPtr statement();
    StmtPtr ifStatement();
    StmtPtr whileStatement();
    StmtPtr forStatement();
    StmtPtr returnStatement();
    StmtPtr tryStatement();
    StmtPtr throwStatement();
    StmtPtr matchStatement();
    std::unique_ptr<BlockStmt> block();
    StmtPtr expressionStatement();

    // --- expressions (precedence climbing) ---
    ExprPtr expression();
    ExprPtr assignment();
    ExprPtr conditional();   // ternary ?:
    ExprPtr logicOr();
    ExprPtr logicAnd();
    ExprPtr bitOr();
    ExprPtr bitXor();
    ExprPtr bitAnd();
    ExprPtr equality();
    ExprPtr comparison();
    ExprPtr shift();
    ExprPtr term();
    ExprPtr factor();
    ExprPtr unary();
    ExprPtr call();
    ExprPtr finishCall(ExprPtr callee);
    ExprPtr primary();
};

} // namespace bee
