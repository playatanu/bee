#pragma once
#include <string>

namespace bee {

enum class TokenType {
    // Literals
    NUMBER, STRING, IDENTIFIER,
    TRUE, FALSE, NIL,

    // Keywords
    LET, CONST, FN, RETURN, IF, ELSE, WHILE, FOR, IN,
    CLASS, EXTENDS, THIS, SUPER, NEW, STATIC,
    IMPORT, FROM, AS,
    BREAK, CONTINUE,
    TRY, CATCH, FINALLY, THROW,
    MATCH, CASE, DEFAULT,
    AND, OR, NOT,

    // Punctuation / operators
    LPAREN, RPAREN, LBRACE, RBRACE, LBRACKET, RBRACKET,
    COMMA, DOT, SEMICOLON, COLON, QUESTION, ELLIPSIS,
    PLUS, MINUS, STAR, SLASH, PERCENT,
    ASSIGN,
    EQ, NEQ, LT, GT, LE, GE,
    PLUS_EQ, MINUS_EQ, STAR_EQ, SLASH_EQ,
    PLUS_PLUS, MINUS_MINUS,
    BIT_AND, BIT_OR, BIT_XOR, BIT_NOT, SHL, SHR,

    EOF_TOK
};

struct Token {
    TokenType type;
    std::string lexeme;   // raw text (for identifiers/strings the decoded value)
    double number = 0.0;  // valid when type == NUMBER
    int line = 0;

    Token() : type(TokenType::EOF_TOK) {}
    Token(TokenType t, std::string lex, int ln)
        : type(t), lexeme(std::move(lex)), line(ln) {}
};

} // namespace bee
