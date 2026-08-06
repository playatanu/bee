#include "lexer.hpp"
#include <cctype>
#include <unordered_map>

namespace bee {

static const std::unordered_map<std::string, TokenType> KEYWORDS = {
    {"let", TokenType::LET},         {"const", TokenType::CONST},
    {"static", TokenType::STATIC},   {"match", TokenType::MATCH},
    {"case", TokenType::CASE},       {"default", TokenType::DEFAULT},
    {"fn", TokenType::FN},
    {"return", TokenType::RETURN},   {"if", TokenType::IF},
    {"else", TokenType::ELSE},       {"while", TokenType::WHILE},
    {"for", TokenType::FOR},         {"in", TokenType::IN},
    {"class", TokenType::CLASS},     {"extends", TokenType::EXTENDS},
    {"this", TokenType::THIS},       {"super", TokenType::SUPER},
    {"new", TokenType::NEW},         {"import", TokenType::IMPORT},
    {"from", TokenType::FROM},       {"as", TokenType::AS},
    {"break", TokenType::BREAK},     {"continue", TokenType::CONTINUE},
    {"try", TokenType::TRY},         {"catch", TokenType::CATCH},
    {"finally", TokenType::FINALLY}, {"throw", TokenType::THROW},
    {"and", TokenType::AND},         {"or", TokenType::OR},
    {"not", TokenType::NOT},         {"true", TokenType::TRUE},
    {"false", TokenType::FALSE},     {"nil", TokenType::NIL},
};

bool Lexer::match(char expected) {
    if (atEnd() || src[pos] != expected) return false;
    pos++;
    return true;
}

void Lexer::addString(std::vector<Token>& out, char quote) {
    std::string value;
    while (!atEnd() && peek() != quote) {
        char c = advance();
        if (c == '\n') line++;
        if (c == '\\' && !atEnd()) {
            char e = advance();
            switch (e) {
                case 'n': value.push_back('\n'); break;
                case 't': value.push_back('\t'); break;
                case 'r': value.push_back('\r'); break;
                case '\\': value.push_back('\\'); break;
                case '"': value.push_back('"'); break;
                case '\'': value.push_back('\''); break;
                case '0': value.push_back('\0'); break;
                default: value.push_back(e); break;
            }
        } else {
            value.push_back(c);
        }
    }
    if (atEnd()) throw LexError("unterminated string", line);
    advance(); // closing quote
    Token t(TokenType::STRING, value, line);
    out.push_back(t);
}

void Lexer::addNumber(std::vector<Token>& out) {
    size_t start = pos - 1;
    while (std::isdigit((unsigned char)peek())) advance();
    if (peek() == '.' && std::isdigit((unsigned char)peekNext())) {
        advance();
        while (std::isdigit((unsigned char)peek())) advance();
    }
    std::string text = src.substr(start, pos - start);
    Token t(TokenType::NUMBER, text, line);
    t.number = std::stod(text);
    out.push_back(t);
}

void Lexer::addIdentifier(std::vector<Token>& out) {
    size_t start = pos - 1;
    while (std::isalnum((unsigned char)peek()) || peek() == '_') advance();
    std::string text = src.substr(start, pos - start);
    auto it = KEYWORDS.find(text);
    TokenType type = (it != KEYWORDS.end()) ? it->second : TokenType::IDENTIFIER;
    out.emplace_back(type, text, line);
}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> out;
    while (!atEnd()) {
        char c = advance();
        switch (c) {
            case ' ': case '\r': case '\t': break;
            case '\n': line++; break;
            case '#': // line comment
                while (!atEnd() && peek() != '\n') advance();
                break;
            case '(': out.emplace_back(TokenType::LPAREN, "(", line); break;
            case ')': out.emplace_back(TokenType::RPAREN, ")", line); break;
            case '{': out.emplace_back(TokenType::LBRACE, "{", line); break;
            case '}': out.emplace_back(TokenType::RBRACE, "}", line); break;
            case '[': out.emplace_back(TokenType::LBRACKET, "[", line); break;
            case ']': out.emplace_back(TokenType::RBRACKET, "]", line); break;
            case ',': out.emplace_back(TokenType::COMMA, ",", line); break;
            case '.':
                if (peek() == '.' && peekNext() == '.') { advance(); advance(); out.emplace_back(TokenType::ELLIPSIS, "...", line); }
                else out.emplace_back(TokenType::DOT, ".", line);
                break;
            case ';': out.emplace_back(TokenType::SEMICOLON, ";", line); break;
            case ':': out.emplace_back(TokenType::COLON, ":", line); break;
            case '?': out.emplace_back(TokenType::QUESTION, "?", line); break;
            case '^': out.emplace_back(TokenType::BIT_XOR, "^", line); break;
            case '~': out.emplace_back(TokenType::BIT_NOT, "~", line); break;
            case '+':
                if (match('+')) out.emplace_back(TokenType::PLUS_PLUS, "++", line);
                else out.emplace_back(match('=') ? TokenType::PLUS_EQ : TokenType::PLUS, "+", line);
                break;
            case '-':
                if (match('-')) out.emplace_back(TokenType::MINUS_MINUS, "--", line);
                else out.emplace_back(match('=') ? TokenType::MINUS_EQ : TokenType::MINUS, "-", line);
                break;
            case '*': out.emplace_back(match('=') ? TokenType::STAR_EQ : TokenType::STAR, "*", line); break;
            case '/':
                if (match('/')) { // alternate line comment
                    while (!atEnd() && peek() != '\n') advance();
                } else {
                    out.emplace_back(match('=') ? TokenType::SLASH_EQ : TokenType::SLASH, "/", line);
                }
                break;
            case '%': out.emplace_back(TokenType::PERCENT, "%", line); break;
            case '=': out.emplace_back(match('=') ? TokenType::EQ : TokenType::ASSIGN, "=", line); break;
            case '!':
                if (match('=')) out.emplace_back(TokenType::NEQ, "!=", line);
                else out.emplace_back(TokenType::NOT, "!", line);
                break;
            case '<':
                if (match('<')) out.emplace_back(TokenType::SHL, "<<", line);
                else out.emplace_back(match('=') ? TokenType::LE : TokenType::LT, "<", line);
                break;
            case '>':
                if (match('>')) out.emplace_back(TokenType::SHR, ">>", line);
                else out.emplace_back(match('=') ? TokenType::GE : TokenType::GT, ">", line);
                break;
            case '&': out.emplace_back(match('&') ? TokenType::AND : TokenType::BIT_AND, "&", line); break;
            case '|': out.emplace_back(match('|') ? TokenType::OR : TokenType::BIT_OR, "|", line); break;
            case '"': addString(out, '"'); break;
            case '\'': addString(out, '\''); break;
            default:
                if (std::isdigit((unsigned char)c)) { pos--; advance(); addNumber(out); }
                else if (std::isalpha((unsigned char)c) || c == '_') { addIdentifier(out); }
                else throw LexError(std::string("unexpected character '") + c + "'", line);
                break;
        }
    }
    out.emplace_back(TokenType::EOF_TOK, "", line);
    return out;
}

} // namespace bee
