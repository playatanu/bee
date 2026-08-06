#include "parser.hpp"

namespace bee {

bool Parser::matchAny(std::initializer_list<TokenType> types) {
    for (auto t : types) if (check(t)) { advance(); return true; }
    return false;
}

const Token& Parser::consume(TokenType t, const std::string& msg) {
    if (check(t)) return advance();
    throw ParseError(msg + " (got '" + peek().lexeme + "')", peek().line);
}

Program Parser::parse() {
    Program program;
    while (!atEnd()) {
        program.push_back(declaration());
    }
    return program;
}

// ---------------- Statements ----------------

StmtPtr Parser::declaration() {
    if (match(TokenType::LET))    return letStatement(false);
    if (match(TokenType::CONST))  return letStatement(true);
    if (match(TokenType::FN))     return functionDecl("function");
    if (match(TokenType::CLASS))  return classDeclaration();
    if (check(TokenType::IMPORT) || check(TokenType::FROM)) return importStatement();
    return statement();
}

StmtPtr Parser::letStatement(bool isConst) {
    auto stmt = std::make_unique<LetStmt>();
    stmt->line = previous().line;
    stmt->isConst = isConst;
    const char* kw = isConst ? "const" : "let";

    if (check(TokenType::LBRACKET) || check(TokenType::LBRACE)) {
        // Destructuring: let [a, b] = ...  or  let {x, y} = ...
        stmt->isDestructure = true;
        stmt->destructureDict = check(TokenType::LBRACE);
        TokenType close = stmt->destructureDict ? TokenType::RBRACE : TokenType::RBRACKET;
        advance(); // consume [ or {
        if (!check(close)) {
            do {
                stmt->names.push_back(consume(TokenType::IDENTIFIER, "expected name in destructuring pattern").lexeme);
            } while (match(TokenType::COMMA));
        }
        consume(close, "expected closing bracket in destructuring pattern");
        consume(TokenType::ASSIGN, std::string("destructuring '") + kw + "' requires an initializer");
        stmt->initializer = expression();
    } else {
        stmt->name = consume(TokenType::IDENTIFIER, std::string("expected variable name after '") + kw + "'").lexeme;
        if (match(TokenType::ASSIGN)) stmt->initializer = expression();
    }
    match(TokenType::SEMICOLON); // optional
    return stmt;
}

// Parse a parenthesized parameter list, including `name = default` and a
// trailing `...rest` parameter.
void Parser::parseParams(FunctionStmt* fn) {
    consume(TokenType::LPAREN, "expected '(' before parameters");
    if (!check(TokenType::RPAREN)) {
        do {
            if (match(TokenType::ELLIPSIS)) {
                std::string rn = consume(TokenType::IDENTIFIER, "expected rest parameter name").lexeme;
                fn->restParam = (int)fn->params.size();
                fn->params.push_back(rn);
                fn->defaults.push_back(nullptr);
                break; // a rest parameter must be last
            }
            std::string pn = consume(TokenType::IDENTIFIER, "expected parameter name").lexeme;
            fn->params.push_back(pn);
            fn->defaults.push_back(match(TokenType::ASSIGN) ? expression() : nullptr);
        } while (match(TokenType::COMMA));
    }
    consume(TokenType::RPAREN, "expected ')' after parameters");
}

std::unique_ptr<FunctionStmt> Parser::functionDecl(const std::string& kind) {
    auto fn = std::make_unique<FunctionStmt>();
    fn->line = previous().line;
    fn->name = consume(TokenType::IDENTIFIER, "expected " + kind + " name").lexeme;
    parseParams(fn.get());
    consume(TokenType::LBRACE, "expected '{' before " + kind + " body");
    auto body = block();
    fn->body = std::move(body->statements);
    return fn;
}

StmtPtr Parser::classDeclaration() {
    auto cls = std::make_unique<ClassStmt>();
    cls->line = previous().line;
    cls->name = consume(TokenType::IDENTIFIER, "expected class name").lexeme;
    if (match(TokenType::EXTENDS)) {
        cls->superclassName = consume(TokenType::IDENTIFIER, "expected superclass name").lexeme;
    }
    consume(TokenType::LBRACE, "expected '{' before class body");
    while (!check(TokenType::RBRACE) && !atEnd()) {
        match(TokenType::FN); // optional 'fn' before method
        cls->methods.push_back(functionDecl("method"));
    }
    consume(TokenType::RBRACE, "expected '}' after class body");
    return cls;
}

StmtPtr Parser::importStatement() {
    auto imp = std::make_unique<ImportStmt>();
    imp->line = peek().line;

    auto dottedPath = [&]() {
        std::string path = consume(TokenType::IDENTIFIER, "expected module name").lexeme;
        std::string last = path;
        while (match(TokenType::DOT)) {
            std::string part = consume(TokenType::IDENTIFIER, "expected name after '.'").lexeme;
            path += "/" + part;
            last = part;
        }
        return std::make_pair(path, last);
    };

    if (match(TokenType::FROM)) {
        imp->isFrom = true;
        auto [path, last] = dottedPath();
        imp->moduleName = path;
        imp->bindName = last;
        consume(TokenType::IMPORT, "expected 'import' after module name");
        if (match(TokenType::STAR)) {
            imp->importAll = true;
        } else {
            do {
                std::string name = consume(TokenType::IDENTIFIER, "expected name to import").lexeme;
                std::string alias = name;
                if (match(TokenType::AS)) alias = consume(TokenType::IDENTIFIER, "expected alias").lexeme;
                imp->names.emplace_back(name, alias);
            } while (match(TokenType::COMMA));
        }
    } else {
        consume(TokenType::IMPORT, "expected 'import'");
        auto [path, last] = dottedPath();
        imp->moduleName = path;
        imp->bindName = last;
        if (match(TokenType::AS)) {
            imp->alias = consume(TokenType::IDENTIFIER, "expected alias after 'as'").lexeme;
            imp->bindName = imp->alias;
        }
    }
    match(TokenType::SEMICOLON);
    return imp;
}

StmtPtr Parser::statement() {
    if (match(TokenType::IF))       return ifStatement();
    if (match(TokenType::WHILE))    return whileStatement();
    if (match(TokenType::FOR))      return forStatement();
    if (match(TokenType::RETURN))   return returnStatement();
    if (match(TokenType::TRY))      return tryStatement();
    if (match(TokenType::THROW))    return throwStatement();
    if (match(TokenType::MATCH))    return matchStatement();
    if (match(TokenType::LBRACE))   return block();
    if (match(TokenType::BREAK))    { match(TokenType::SEMICOLON); return std::make_unique<BreakStmt>(); }
    if (match(TokenType::CONTINUE)) { match(TokenType::SEMICOLON); return std::make_unique<ContinueStmt>(); }
    return expressionStatement();
}

StmtPtr Parser::ifStatement() {
    auto stmt = std::make_unique<IfStmt>();
    stmt->line = previous().line;
    bool paren = match(TokenType::LPAREN);
    stmt->condition = expression();
    if (paren) consume(TokenType::RPAREN, "expected ')' after if condition");
    stmt->thenBranch = statement();
    if (match(TokenType::ELSE)) stmt->elseBranch = statement();
    return stmt;
}

StmtPtr Parser::whileStatement() {
    auto stmt = std::make_unique<WhileStmt>();
    stmt->line = previous().line;
    bool paren = match(TokenType::LPAREN);
    stmt->condition = expression();
    if (paren) consume(TokenType::RPAREN, "expected ')' after while condition");
    stmt->body = statement();
    return stmt;
}

StmtPtr Parser::forStatement() {
    int line = previous().line;
    // Distinguish `for x in iter { }` from C-style `for (...) { }`.
    // Look for: for IDENT in ...
    if (check(TokenType::IDENTIFIER) && tokens[current + 1].type == TokenType::IN) {
        auto stmt = std::make_unique<ForInStmt>();
        stmt->line = line;
        stmt->name = advance().lexeme; // IDENT
        consume(TokenType::IN, "expected 'in'");
        stmt->iterable = expression();
        consume(TokenType::LBRACE, "expected '{' before for body");
        stmt->body = block();
        return stmt;
    }

    auto stmt = std::make_unique<ForStmt>();
    stmt->line = line;
    bool paren = match(TokenType::LPAREN);
    // initializer
    if (match(TokenType::SEMICOLON)) {
        stmt->init = nullptr;
    } else if (match(TokenType::LET)) {
        stmt->init = letStatement();
    } else {
        stmt->init = expressionStatement();
    }
    // condition
    if (!check(TokenType::SEMICOLON)) stmt->condition = expression();
    consume(TokenType::SEMICOLON, "expected ';' after for condition");
    // increment
    if (paren) {
        if (!check(TokenType::RPAREN)) stmt->increment = expression();
        consume(TokenType::RPAREN, "expected ')' after for clauses");
    } else {
        if (!check(TokenType::LBRACE)) stmt->increment = expression();
    }
    consume(TokenType::LBRACE, "expected '{' before for body");
    stmt->body = block();
    return stmt;
}

StmtPtr Parser::returnStatement() {
    auto stmt = std::make_unique<ReturnStmt>();
    stmt->line = previous().line;
    if (!check(TokenType::SEMICOLON) && !check(TokenType::RBRACE)) {
        stmt->value = expression();
    }
    match(TokenType::SEMICOLON);
    return stmt;
}

StmtPtr Parser::tryStatement() {
    auto stmt = std::make_unique<TryStmt>();
    stmt->line = previous().line;
    consume(TokenType::LBRACE, "expected '{' after 'try'");
    stmt->body = block();

    if (match(TokenType::CATCH)) {
        stmt->hasCatch = true;
        // Optional binding: `catch (e)`, `catch e`, or bare `catch`.
        if (match(TokenType::LPAREN)) {
            if (check(TokenType::IDENTIFIER))
                stmt->catchName = advance().lexeme;
            consume(TokenType::RPAREN, "expected ')' after catch variable");
        } else if (check(TokenType::IDENTIFIER)) {
            stmt->catchName = advance().lexeme;
        }
        consume(TokenType::LBRACE, "expected '{' before catch body");
        stmt->catchBody = block();
    }

    if (match(TokenType::FINALLY)) {
        stmt->hasFinally = true;
        consume(TokenType::LBRACE, "expected '{' before finally body");
        stmt->finallyBody = block();
    }

    if (!stmt->hasCatch && !stmt->hasFinally)
        throw ParseError("'try' must have a 'catch' or 'finally' block", stmt->line);
    return stmt;
}

StmtPtr Parser::throwStatement() {
    auto stmt = std::make_unique<ThrowStmt>();
    stmt->line = previous().line;
    stmt->value = expression();
    match(TokenType::SEMICOLON);
    return stmt;
}

StmtPtr Parser::matchStatement() {
    auto stmt = std::make_unique<MatchStmt>();
    stmt->line = previous().line;
    stmt->subject = expression();
    consume(TokenType::LBRACE, "expected '{' after match subject");
    while (!check(TokenType::RBRACE) && !atEnd()) {
        if (match(TokenType::CASE)) {
            MatchCase mc;
            do {
                mc.values.push_back(expression());
            } while (match(TokenType::COMMA));
            consume(TokenType::LBRACE, "expected '{' after case values");
            mc.body = block();
            stmt->cases.push_back(std::move(mc));
        } else if (match(TokenType::DEFAULT)) {
            if (stmt->hasDefault) throw ParseError("duplicate 'default' in match", previous().line);
            stmt->hasDefault = true;
            consume(TokenType::LBRACE, "expected '{' after 'default'");
            stmt->defaultBody = block();
        } else {
            throw ParseError("expected 'case' or 'default' in match body", peek().line);
        }
    }
    consume(TokenType::RBRACE, "expected '}' after match body");
    return stmt;
}

std::unique_ptr<BlockStmt> Parser::block() {
    auto blk = std::make_unique<BlockStmt>();
    blk->line = previous().line;
    while (!check(TokenType::RBRACE) && !atEnd()) {
        blk->statements.push_back(declaration());
    }
    consume(TokenType::RBRACE, "expected '}' after block");
    return blk;
}

StmtPtr Parser::expressionStatement() {
    auto stmt = std::make_unique<ExprStmt>();
    stmt->line = peek().line;
    stmt->expr = expression();
    match(TokenType::SEMICOLON); // optional
    return stmt;
}

// ---------------- Expressions ----------------

ExprPtr Parser::expression() { return assignment(); }

// Map a compound-assignment token (+=) to its arithmetic op (+), or ASSIGN.
static TokenType compoundOp(TokenType t) {
    switch (t) {
        case TokenType::PLUS_EQ:  return TokenType::PLUS;
        case TokenType::MINUS_EQ: return TokenType::MINUS;
        case TokenType::STAR_EQ:  return TokenType::STAR;
        case TokenType::SLASH_EQ: return TokenType::SLASH;
        default:                  return TokenType::ASSIGN;
    }
}

ExprPtr Parser::assignment() {
    ExprPtr expr = conditional();

    if (matchAny({TokenType::ASSIGN, TokenType::PLUS_EQ, TokenType::MINUS_EQ,
                  TokenType::STAR_EQ, TokenType::SLASH_EQ})) {
        TokenType opTok = previous().type;
        TokenType arith = compoundOp(opTok);
        int line = previous().line;
        ExprPtr rhs = assignment();

        if (expr->kind == Expr::Kind::Variable) {
            auto* var = static_cast<VariableExpr*>(expr.get());
            auto assign = std::make_unique<AssignExpr>();
            assign->line = line;
            assign->name = var->name;
            if (opTok == TokenType::ASSIGN) {
                assign->value = std::move(rhs);
            } else {
                // Desugar `x += y` into `x = x <op> y`.
                auto bin = std::make_unique<BinaryExpr>();
                bin->line = line;
                bin->op = arith;
                bin->left = std::make_unique<VariableExpr>(var->name);
                bin->right = std::move(rhs);
                assign->value = std::move(bin);
            }
            return assign;
        } else if (expr->kind == Expr::Kind::Get) {
            // Compound handled in the interpreter (object evaluated once via `op`).
            auto* get = static_cast<GetExpr*>(expr.get());
            auto set = std::make_unique<SetExpr>();
            set->line = line;
            set->name = get->name;
            set->object = std::move(get->object);
            set->value = std::move(rhs);
            set->op = arith;
            return set;
        } else if (expr->kind == Expr::Kind::Index) {
            auto* idx = static_cast<IndexExpr*>(expr.get());
            auto set = std::make_unique<IndexSetExpr>();
            set->line = line;
            set->object = std::move(idx->object);
            set->index = std::move(idx->index);
            set->value = std::move(rhs);
            set->op = arith;
            return set;
        }
        throw ParseError("invalid assignment target", line);
    }
    return expr;
}

ExprPtr Parser::conditional() {
    ExprPtr cond = logicOr();
    if (match(TokenType::QUESTION)) {
        auto t = std::make_unique<TernaryExpr>();
        t->line = previous().line;
        t->cond = std::move(cond);
        t->thenBranch = expression();
        consume(TokenType::COLON, "expected ':' in ternary expression");
        t->elseBranch = conditional();
        return t;
    }
    return cond;
}

ExprPtr Parser::logicOr() {
    ExprPtr expr = logicAnd();
    while (match(TokenType::OR)) {
        auto l = std::make_unique<LogicalExpr>();
        l->line = previous().line;
        l->op = TokenType::OR;
        l->left = std::move(expr);
        l->right = logicAnd();
        expr = std::move(l);
    }
    return expr;
}

ExprPtr Parser::logicAnd() {
    ExprPtr expr = bitOr();
    while (match(TokenType::AND)) {
        auto l = std::make_unique<LogicalExpr>();
        l->line = previous().line;
        l->op = TokenType::AND;
        l->left = std::move(expr);
        l->right = bitOr();
        expr = std::move(l);
    }
    return expr;
}

// Bitwise: | then ^ then & (looser to tighter), all above equality.
ExprPtr Parser::bitOr() {
    ExprPtr expr = bitXor();
    while (match(TokenType::BIT_OR)) {
        auto b = std::make_unique<BinaryExpr>();
        b->line = previous().line;
        b->op = TokenType::BIT_OR;
        b->left = std::move(expr);
        b->right = bitXor();
        expr = std::move(b);
    }
    return expr;
}

ExprPtr Parser::bitXor() {
    ExprPtr expr = bitAnd();
    while (match(TokenType::BIT_XOR)) {
        auto b = std::make_unique<BinaryExpr>();
        b->line = previous().line;
        b->op = TokenType::BIT_XOR;
        b->left = std::move(expr);
        b->right = bitAnd();
        expr = std::move(b);
    }
    return expr;
}

ExprPtr Parser::bitAnd() {
    ExprPtr expr = equality();
    while (match(TokenType::BIT_AND)) {
        auto b = std::make_unique<BinaryExpr>();
        b->line = previous().line;
        b->op = TokenType::BIT_AND;
        b->left = std::move(expr);
        b->right = equality();
        expr = std::move(b);
    }
    return expr;
}

ExprPtr Parser::equality() {
    ExprPtr expr = comparison();
    while (matchAny({TokenType::EQ, TokenType::NEQ})) {
        auto b = std::make_unique<BinaryExpr>();
        b->line = previous().line;
        b->op = previous().type;
        b->left = std::move(expr);
        b->right = comparison();
        expr = std::move(b);
    }
    return expr;
}

ExprPtr Parser::comparison() {
    ExprPtr expr = shift();
    while (matchAny({TokenType::LT, TokenType::GT, TokenType::LE, TokenType::GE})) {
        auto b = std::make_unique<BinaryExpr>();
        b->line = previous().line;
        b->op = previous().type;
        b->left = std::move(expr);
        b->right = shift();
        expr = std::move(b);
    }
    return expr;
}

ExprPtr Parser::shift() {
    ExprPtr expr = term();
    while (matchAny({TokenType::SHL, TokenType::SHR})) {
        auto b = std::make_unique<BinaryExpr>();
        b->line = previous().line;
        b->op = previous().type;
        b->left = std::move(expr);
        b->right = term();
        expr = std::move(b);
    }
    return expr;
}

ExprPtr Parser::term() {
    ExprPtr expr = factor();
    while (matchAny({TokenType::PLUS, TokenType::MINUS})) {
        auto b = std::make_unique<BinaryExpr>();
        b->line = previous().line;
        b->op = previous().type;
        b->left = std::move(expr);
        b->right = factor();
        expr = std::move(b);
    }
    return expr;
}

ExprPtr Parser::factor() {
    ExprPtr expr = unary();
    while (matchAny({TokenType::STAR, TokenType::SLASH, TokenType::PERCENT})) {
        auto b = std::make_unique<BinaryExpr>();
        b->line = previous().line;
        b->op = previous().type;
        b->left = std::move(expr);
        b->right = unary();
        expr = std::move(b);
    }
    return expr;
}

// Desugar `x++` / `++x` (and --) into a compound assignment by 1.
static ExprPtr makeIncDec(ExprPtr target, TokenType arith, int line) {
    if (target->kind == Expr::Kind::Variable) {
        auto* var = static_cast<VariableExpr*>(target.get());
        auto assign = std::make_unique<AssignExpr>();
        assign->line = line;
        assign->name = var->name;
        auto bin = std::make_unique<BinaryExpr>();
        bin->line = line;
        bin->op = arith;
        bin->left = std::make_unique<VariableExpr>(var->name);
        bin->right = std::make_unique<LiteralExpr>(Value(1.0));
        assign->value = std::move(bin);
        return assign;
    } else if (target->kind == Expr::Kind::Get) {
        auto* get = static_cast<GetExpr*>(target.get());
        auto set = std::make_unique<SetExpr>();
        set->line = line;
        set->name = get->name;
        set->op = arith;
        set->object = std::move(get->object);
        set->value = std::make_unique<LiteralExpr>(Value(1.0));
        return set;
    } else if (target->kind == Expr::Kind::Index) {
        auto* idx = static_cast<IndexExpr*>(target.get());
        auto set = std::make_unique<IndexSetExpr>();
        set->line = line;
        set->op = arith;
        set->object = std::move(idx->object);
        set->index = std::move(idx->index);
        set->value = std::make_unique<LiteralExpr>(Value(1.0));
        return set;
    }
    throw ParseError("invalid increment/decrement target", line);
}

ExprPtr Parser::unary() {
    if (matchAny({TokenType::PLUS_PLUS, TokenType::MINUS_MINUS})) {
        TokenType t = previous().type;
        int line = previous().line;
        return makeIncDec(unary(), t == TokenType::PLUS_PLUS ? TokenType::PLUS : TokenType::MINUS, line);
    }
    if (matchAny({TokenType::NOT, TokenType::MINUS, TokenType::BIT_NOT})) {
        auto u = std::make_unique<UnaryExpr>();
        u->line = previous().line;
        u->op = previous().type;
        u->right = unary();
        return u;
    }
    return call();
}

ExprPtr Parser::finishCall(ExprPtr callee) {
    auto c = std::make_unique<CallExpr>();
    c->line = previous().line;
    c->callee = std::move(callee);
    if (!check(TokenType::RPAREN)) {
        do {
            bool sp = match(TokenType::ELLIPSIS);
            c->args.push_back(expression());
            c->spread.push_back(sp);
        } while (match(TokenType::COMMA));
    }
    consume(TokenType::RPAREN, "expected ')' after arguments");
    return c;
}

ExprPtr Parser::call() {
    ExprPtr expr = primary();
    while (true) {
        if (match(TokenType::LPAREN)) {
            expr = finishCall(std::move(expr));
        } else if (match(TokenType::DOT)) {
            auto get = std::make_unique<GetExpr>();
            get->line = previous().line;
            get->name = consume(TokenType::IDENTIFIER, "expected property name after '.'").lexeme;
            get->object = std::move(expr);
            expr = std::move(get);
        } else if (match(TokenType::LBRACKET)) {
            auto idx = std::make_unique<IndexExpr>();
            idx->line = previous().line;
            idx->object = std::move(expr);
            idx->index = expression();
            consume(TokenType::RBRACKET, "expected ']' after index");
            expr = std::move(idx);
        } else {
            break;
        }
    }
    if (matchAny({TokenType::PLUS_PLUS, TokenType::MINUS_MINUS})) {
        TokenType t = previous().type;
        expr = makeIncDec(std::move(expr), t == TokenType::PLUS_PLUS ? TokenType::PLUS : TokenType::MINUS, previous().line);
    }
    return expr;
}

ExprPtr Parser::primary() {
    int line = peek().line;
    if (match(TokenType::TRUE))  return std::make_unique<LiteralExpr>(Value(true));
    if (match(TokenType::FALSE)) return std::make_unique<LiteralExpr>(Value(false));
    if (match(TokenType::NIL))   return std::make_unique<LiteralExpr>(Value(nullptr));
    if (match(TokenType::NUMBER)) return std::make_unique<LiteralExpr>(Value(previous().number));
    if (match(TokenType::STRING)) return std::make_unique<LiteralExpr>(Value(previous().lexeme));

    if (match(TokenType::THIS)) { auto t = std::make_unique<ThisExpr>(); t->line = line; return t; }

    if (match(TokenType::FN)) {
        // Anonymous function: fn (params) { body }
        auto fe = std::make_unique<FunctionExpr>();
        fe->line = line;
        auto fn = std::make_unique<FunctionStmt>();
        fn->line = line;
        parseParams(fn.get());
        consume(TokenType::LBRACE, "expected '{' before function body");
        auto body = block();
        fn->body = std::move(body->statements);
        fe->fn = std::move(fn);
        return fe;
    }

    if (match(TokenType::SUPER)) {
        consume(TokenType::DOT, "expected '.' after 'super'");
        auto s = std::make_unique<SuperExpr>();
        s->line = line;
        s->method = consume(TokenType::IDENTIFIER, "expected method name after 'super.'").lexeme;
        return s;
    }

    if (match(TokenType::NEW)) {
        // `new Class(args)` is sugar for `Class(args)`.
        return call();
    }

    if (match(TokenType::IDENTIFIER)) {
        return std::make_unique<VariableExpr>(previous().lexeme);
    }

    if (match(TokenType::LPAREN)) {
        auto g = std::make_unique<GroupingExpr>();
        g->line = line;
        g->inner = expression();
        consume(TokenType::RPAREN, "expected ')' after expression");
        return g;
    }

    if (match(TokenType::LBRACKET)) {
        if (match(TokenType::RBRACKET)) {   // empty list
            auto list = std::make_unique<ListLitExpr>();
            list->line = line;
            return list;
        }
        bool firstSpread = match(TokenType::ELLIPSIS);
        ExprPtr first = expression();

        // List comprehension: [ elem for name in iterable (if cond)? ]
        if (!firstSpread && match(TokenType::FOR)) {
            auto comp = std::make_unique<ListCompExpr>();
            comp->line = line;
            comp->elem = std::move(first);
            comp->name = consume(TokenType::IDENTIFIER, "expected variable name after 'for'").lexeme;
            consume(TokenType::IN, "expected 'in' in list comprehension");
            comp->iterable = expression();
            if (match(TokenType::IF)) comp->cond = expression();
            consume(TokenType::RBRACKET, "expected ']' after comprehension");
            return comp;
        }

        auto list = std::make_unique<ListLitExpr>();
        list->line = line;
        list->elements.push_back(std::move(first));
        list->spread.push_back(firstSpread);
        while (match(TokenType::COMMA)) {
            if (check(TokenType::RBRACKET)) break; // trailing comma
            bool sp = match(TokenType::ELLIPSIS);
            list->elements.push_back(expression());
            list->spread.push_back(sp);
        }
        consume(TokenType::RBRACKET, "expected ']' after list");
        return list;
    }

    if (match(TokenType::LBRACE)) {
        auto dict = std::make_unique<DictLitExpr>();
        dict->line = line;
        if (!check(TokenType::RBRACE)) {
            do {
                if (check(TokenType::RBRACE)) break;
                ExprPtr key = expression();
                consume(TokenType::COLON, "expected ':' in dict entry");
                ExprPtr val = expression();
                dict->entries.emplace_back(std::move(key), std::move(val));
            } while (match(TokenType::COMMA));
        }
        consume(TokenType::RBRACE, "expected '}' after dict");
        return dict;
    }

    throw ParseError("expected expression", line);
}

} // namespace bee
