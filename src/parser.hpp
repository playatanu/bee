#pragma once
#include "token.hpp"
#include "ast.hpp"
#include <vector>
#include <stdexcept>

namespace bee {

struct ParseError : std::runtime_error {
    int line;
    ParseError(const std::string& msg, int ln)
        : std::runtime_error("Parse error (line " + std::to_string(ln) + "): " + msg), line(ln) {}
};

class Parser {
public:
    explicit Parser(std::vector<Token> toks) : tokens(std::move(toks)) {}
    Program parse();

private:
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
