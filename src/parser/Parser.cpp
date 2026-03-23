#include "ferrum/Parser.h"
#include <sstream>

namespace ferrum {

Parser::Parser(std::vector<Token> toks) : tokens(std::move(toks)) {}

// ── Token helpers ─────────────────────────────────────────────────────────────

Token& Parser::peek(int offset) {
    size_t idx = pos + offset;
    if (idx >= tokens.size()) return tokens.back(); // EOF
    return tokens[idx];
}

Token& Parser::advance() {
    if (!isAtEnd()) pos++;
    return tokens[pos - 1];
}

bool Parser::check(TokenKind k) const {
    if (pos >= tokens.size()) return false;
    return tokens[pos].kind == k;
}

bool Parser::match(TokenKind k) {
    if (check(k)) { advance(); return true; }
    return false;
}

Token Parser::expect(TokenKind k, const std::string& msg) {
    if (check(k)) return advance();
    throw error(msg);
}

bool Parser::isAtEnd() const {
    return pos >= tokens.size() || tokens[pos].kind == TokenKind::EOF_TOK;
}

ParseError Parser::error(const std::string& msg) {
    auto& t = peek();
    std::ostringstream oss;
    oss << "[" << t.line << ":" << t.col << "] " << msg
        << " (got '" << t.lexeme << "')";
    return ParseError(oss.str(), t.line, t.col);
}

// ── Program ───────────────────────────────────────────────────────────────────

Program Parser::parse() {
    Program prog;
    while (!isAtEnd()) {
        prog.decls.push_back(parseDecl());
    }
    return prog;
}

// ── Declarations ──────────────────────────────────────────────────────────────

DeclPtr Parser::parseDecl() {
    if (check(TokenKind::KW_IMPORT))  return parseImportDecl();
    if (check(TokenKind::KW_EXTERN))  return parseExternBlock();
    if (check(TokenKind::KW_STRUCT))  return parseStructDecl();
    // Everything else must start with a type → function
    return parseFunctionDecl();
}

DeclPtr Parser::parseFunctionDecl() {
    auto decl = std::make_unique<Decl>();
    decl->kind = Decl::Kind::Function;
    decl->line = peek().line;

    if (match(TokenKind::KW_UNSAFE)) decl->isUnsafe = true;

    decl->returnType = std::make_unique<TypeRef>(parseType());
    decl->funcName = expect(TokenKind::IDENT, "Expected function name").lexeme;

    // Optional generic type params: fn foo<T, U>(...)
    if (check(TokenKind::LT)) {
        advance();
        while (!check(TokenKind::GT) && !isAtEnd()) {
            decl->typeParams.push_back(expect(TokenKind::IDENT, "Expected type parameter name").lexeme);
            if (!check(TokenKind::GT)) expect(TokenKind::COMMA, "Expected ',' between type params");
        }
        expect(TokenKind::GT, "Expected '>' to close type parameter list");
    }

    expect(TokenKind::LPAREN, "Expected '(' after function name");
    while (!check(TokenKind::RPAREN) && !isAtEnd()) {
        Param p;
        p.type = parseType();
        p.name = expect(TokenKind::IDENT, "Expected parameter name").lexeme;
        decl->params.push_back(std::move(p));
        if (!check(TokenKind::RPAREN)) expect(TokenKind::COMMA, "Expected ',' between params");
    }
    expect(TokenKind::RPAREN, "Expected ')' after parameters");

    decl->funcBody = parseBlock();
    return decl;
}

DeclPtr Parser::parseStructDecl() {
    auto decl = std::make_unique<Decl>();
    decl->kind = Decl::Kind::Struct;
    decl->line = peek().line;

    expect(TokenKind::KW_STRUCT, "Expected 'struct'");
    decl->structName = expect(TokenKind::IDENT, "Expected struct name").lexeme;

    // Optional generic type params: struct Pair<T, U> { ... }
    if (check(TokenKind::LT)) {
        advance();
        while (!check(TokenKind::GT) && !isAtEnd()) {
            decl->structTypeParams.push_back(expect(TokenKind::IDENT, "Expected type parameter name").lexeme);
            if (!check(TokenKind::GT)) expect(TokenKind::COMMA, "Expected ',' between type params");
        }
        expect(TokenKind::GT, "Expected '>' to close type parameter list");
    }

    expect(TokenKind::LBRACE, "Expected '{' after struct name");

    while (!check(TokenKind::RBRACE) && !isAtEnd()) {
        Param f;
        f.type = parseType();
        f.name = expect(TokenKind::IDENT, "Expected field name").lexeme;
        expect(TokenKind::SEMICOLON, "Expected ';' after field");
        decl->fields.push_back(std::move(f));
    }
    expect(TokenKind::RBRACE, "Expected '}' to close struct");
    return decl;
}

DeclPtr Parser::parseExternBlock() {
    auto decl = std::make_unique<Decl>();
    decl->kind = Decl::Kind::ExternBlock;
    decl->line = peek().line;

    expect(TokenKind::KW_EXTERN, "Expected 'extern'");
    if (check(TokenKind::STRING_LIT)) {
        decl->externLang = std::get<std::string>(advance().value);
    }
    expect(TokenKind::LBRACE, "Expected '{' for extern block");
    while (!check(TokenKind::RBRACE) && !isAtEnd()) {
        decl->externDecls.push_back(parseFunctionDecl());
    }
    expect(TokenKind::RBRACE, "Expected '}' to close extern block");
    return decl;
}

DeclPtr Parser::parseImportDecl() {
    auto decl = std::make_unique<Decl>();
    decl->kind = Decl::Kind::Import;
    decl->line = peek().line;

    expect(TokenKind::KW_IMPORT, "Expected 'import'");
    if (check(TokenKind::LT)) {
        // import <vector> style (C++ header)
        advance();
        std::string path;
        while (!check(TokenKind::GT) && !isAtEnd()) path += advance().lexeme;
        expect(TokenKind::GT, "Expected '>' to close import");
        decl->importPath = path;
        decl->isCppHeader = true;
    } else if (check(TokenKind::STRING_LIT)) {
        decl->importPath = std::get<std::string>(advance().value);
        decl->isCppHeader = false;
    } else {
        throw error("Expected import path");
    }
    expect(TokenKind::SEMICOLON, "Expected ';' after import");
    return decl;
}

// ── Types ─────────────────────────────────────────────────────────────────────

TypeRef Parser::parseType() {
    if (nestingDepth >= MAX_NESTING_DEPTH)
        throw error("type too deeply nested (max depth: 512)");
    nestingDepth++;
    struct DepthGuard { int& d; ~DepthGuard() { --d; } } guard{nestingDepth};

    bool isUnsafe = false;
    if (match(TokenKind::KW_UNSAFE)) isUnsafe = true;

    // Borrow: &T, &mut T, &'a T, &'a mut T
    if (check(TokenKind::AMP)) {
        advance();
        std::string lt;
        if (check(TokenKind::LIFETIME)) {
            lt = advance().lexeme;  // e.g. "'a"
        }
        bool mut = match(TokenKind::KW_MUT);
        TypeRef inner = parseType();
        return TypeRef::borrow(std::move(inner), mut, std::move(lt));
    }
    if (check(TokenKind::AMP_MUT)) {
        advance();
        std::string lt;
        if (check(TokenKind::LIFETIME)) {
            lt = advance().lexeme;
        }
        TypeRef inner = parseType();
        return TypeRef::borrow(std::move(inner), true, std::move(lt));
    }

    // Pointer: T*
    std::string name;
    if (check(TokenKind::KW_INT))   { advance(); name = "int"; }
    else if (check(TokenKind::KW_FLOAT)) { advance(); name = "float"; }
    else if (check(TokenKind::KW_CHAR))  { advance(); name = "char"; }
    else if (check(TokenKind::KW_VOID))  { advance(); name = "void"; }
    else if (check(TokenKind::KW_BOOL))  { advance(); name = "bool"; }
    else if (check(TokenKind::IDENT))    { name = advance().lexeme; }
    else throw error("Expected type");

    TypeRef base = TypeRef::named(name);

    // Generic type args: Vec<int>, Pair<T, U>
    // Heuristic: if next is '<' and we see IDENT/keyword inside before '>', treat as type args
    if (check(TokenKind::LT)) {
        // Lookahead: peek to decide if this is a generic type arg list or a comparison '<'
        // We commit if what follows looks like a type (keyword or IDENT), not an expression
        size_t saved = pos;
        bool isGeneric = false;
        advance(); // consume '<'
        if (check(TokenKind::KW_INT) || check(TokenKind::KW_FLOAT) || check(TokenKind::KW_CHAR) ||
            check(TokenKind::KW_VOID) || check(TokenKind::KW_BOOL) || check(TokenKind::IDENT) ||
            check(TokenKind::AMP) || check(TokenKind::STAR)) {
            isGeneric = true;
        }
        if (isGeneric) {
            while (!check(TokenKind::GT) && !isAtEnd()) {
                base.typeArgs.push_back(parseType());
                if (!check(TokenKind::GT)) expect(TokenKind::COMMA, "Expected ',' between type args");
            }
            expect(TokenKind::GT, "Expected '>' to close type argument list");
        } else {
            pos = saved; // not a generic, backtrack
        }
    }

    // Handle pointer stars: int*, int** unsafe, etc.
    // 'unsafe' can appear before the type (already captured) or after the '*'
    while (check(TokenKind::STAR)) {
        advance();
        bool starUnsafe = isUnsafe;
        if (check(TokenKind::KW_UNSAFE)) { advance(); starUnsafe = true; }
        base = TypeRef::pointer(std::move(base), starUnsafe);
    }

    // Handle reference suffix: int& or int&mut
    if (check(TokenKind::AMP)) {
        advance();
        bool mut = match(TokenKind::KW_MUT);
        std::string lt;
        if (check(TokenKind::LIFETIME)) lt = advance().lexeme;
        return TypeRef::borrow(std::move(base), mut, std::move(lt));
    }
    if (check(TokenKind::AMP_MUT)) {
        advance();
        std::string lt;
        if (check(TokenKind::LIFETIME)) lt = advance().lexeme;
        return TypeRef::borrow(std::move(base), true, std::move(lt));
    }

    return base;
}

// ── Statements ────────────────────────────────────────────────────────────────

StmtPtr Parser::parseStmt() {
    if (check(TokenKind::LBRACE))    return parseBlock();
    if (check(TokenKind::KW_IF))     return parseIf();
    if (check(TokenKind::KW_WHILE))  return parseWhile();
    if (check(TokenKind::KW_FOR))    return parseFor();
    if (check(TokenKind::KW_RETURN)) return parseReturn();
    if (check(TokenKind::KW_UNSAFE)) return parseUnsafeBlock();

    // Could be var decl (starts with type keyword or ident followed by ident)
    bool looksLikeDecl =
        check(TokenKind::KW_INT) || check(TokenKind::KW_FLOAT) ||
        check(TokenKind::KW_CHAR) || check(TokenKind::KW_VOID) ||
        check(TokenKind::KW_BOOL) ||
        (check(TokenKind::IDENT) && pos+1 < tokens.size() &&
         tokens[pos+1].kind == TokenKind::IDENT);

    if (looksLikeDecl) return parseVarDecl();

    // Expression statement
    auto stmt = std::make_unique<Stmt>();
    stmt->kind = Stmt::Kind::Expr;
    stmt->line = peek().line;
    stmt->expr = parseExpr();
    expect(TokenKind::SEMICOLON, "Expected ';' after expression");
    return stmt;
}

StmtPtr Parser::parseBlock() {
    if (nestingDepth >= MAX_NESTING_DEPTH)
        throw error("block too deeply nested (max depth: 512)");
    nestingDepth++;
    auto stmt = std::make_unique<Stmt>();
    stmt->kind = Stmt::Kind::Block;
    stmt->line = peek().line;
    expect(TokenKind::LBRACE, "Expected '{'");
    while (!check(TokenKind::RBRACE) && !isAtEnd()) {
        stmt->stmts.push_back(parseStmt());
    }
    expect(TokenKind::RBRACE, "Expected '}'");
    nestingDepth--;
    return stmt;
}

StmtPtr Parser::parseVarDecl() {
    auto stmt = std::make_unique<Stmt>();
    stmt->kind = Stmt::Kind::VarDecl;
    stmt->line = peek().line;

    stmt->varType = std::make_unique<TypeRef>(parseType());
    stmt->varName = expect(TokenKind::IDENT, "Expected variable name").lexeme;
    if (match(TokenKind::EQ)) stmt->varInit = parseExpr();
    expect(TokenKind::SEMICOLON, "Expected ';' after variable declaration");
    return stmt;
}

StmtPtr Parser::parseIf() {
    auto stmt = std::make_unique<Stmt>();
    stmt->kind = Stmt::Kind::If;
    stmt->line = peek().line;
    expect(TokenKind::KW_IF, "Expected 'if'");
    expect(TokenKind::LPAREN, "Expected '(' after 'if'");
    stmt->condition = parseExpr();
    expect(TokenKind::RPAREN, "Expected ')' after condition");
    stmt->thenBranch = parseStmt();
    if (match(TokenKind::KW_ELSE)) stmt->elseBranch = parseStmt();
    return stmt;
}

StmtPtr Parser::parseWhile() {
    auto stmt = std::make_unique<Stmt>();
    stmt->kind = Stmt::Kind::While;
    stmt->line = peek().line;
    expect(TokenKind::KW_WHILE, "Expected 'while'");
    expect(TokenKind::LPAREN, "Expected '(' after 'while'");
    stmt->condition = parseExpr();
    expect(TokenKind::RPAREN, "Expected ')'");
    stmt->body = parseStmt();
    return stmt;
}

StmtPtr Parser::parseFor() {
    auto stmt = std::make_unique<Stmt>();
    stmt->kind = Stmt::Kind::For;
    stmt->line = peek().line;
    expect(TokenKind::KW_FOR, "Expected 'for'");
    expect(TokenKind::LPAREN, "Expected '('");
    // Init can be a var decl (int i = 0;) or an expression (i = 0;)
    bool initIsDecl =
        check(TokenKind::KW_INT) || check(TokenKind::KW_FLOAT) ||
        check(TokenKind::KW_CHAR) || check(TokenKind::KW_VOID)  ||
        check(TokenKind::KW_BOOL) ||
        (check(TokenKind::IDENT) && pos+1 < tokens.size() &&
         tokens[pos+1].kind == TokenKind::IDENT);
    if (initIsDecl) {
        stmt->init = parseVarDecl();  // includes the ';'
    } else if (!check(TokenKind::SEMICOLON)) {
        auto initStmt = std::make_unique<Stmt>();
        initStmt->kind = Stmt::Kind::Expr;
        initStmt->line = peek().line;
        initStmt->expr = parseExpr();
        expect(TokenKind::SEMICOLON, "Expected ';' after for init");
        stmt->init = std::move(initStmt);
    } else {
        expect(TokenKind::SEMICOLON, "Expected ';'");
    }
    stmt->condition = parseExpr();
    expect(TokenKind::SEMICOLON, "Expected ';'");
    stmt->increment = parseExpr();
    expect(TokenKind::RPAREN, "Expected ')'");
    stmt->body = parseStmt();
    return stmt;
}

StmtPtr Parser::parseReturn() {
    auto stmt = std::make_unique<Stmt>();
    stmt->kind = Stmt::Kind::Return;
    stmt->line = peek().line;
    expect(TokenKind::KW_RETURN, "Expected 'return'");
    if (!check(TokenKind::SEMICOLON)) stmt->expr = parseExpr();
    expect(TokenKind::SEMICOLON, "Expected ';' after return");
    return stmt;
}

StmtPtr Parser::parseUnsafeBlock() {
    auto stmt = std::make_unique<Stmt>();
    stmt->kind = Stmt::Kind::Unsafe;
    stmt->line = peek().line;
    expect(TokenKind::KW_UNSAFE, "Expected 'unsafe'");
    stmt->stmts.push_back(parseBlock());
    return stmt;
}

// ── Expressions ───────────────────────────────────────────────────────────────

ExprPtr Parser::parseExpr() {
    if (nestingDepth >= MAX_NESTING_DEPTH)
        throw error("expression too deeply nested (max depth: 512)");
    nestingDepth++;
    auto result = parseAssign();
    nestingDepth--;
    return result;
}

ExprPtr Parser::parseAssign() {
    auto lhs = parseLogicalOr();
    if (check(TokenKind::EQ)) {
        advance();
        auto rhs = parseAssign();
        auto e = std::make_unique<Expr>();
        e->kind = Expr::Kind::Assign;
        e->op = "=";
        e->lhs = std::move(lhs);
        e->rhs = std::move(rhs);
        return e;
    }
    return lhs;
}

ExprPtr Parser::parseLogicalOr() {
    auto lhs = parseLogicalAnd();
    while (check(TokenKind::PIPE_PIPE)) {
        std::string op = advance().lexeme;
        auto rhs = parseLogicalAnd();
        auto e = std::make_unique<Expr>();
        e->kind = Expr::Kind::BinOp; e->op = op;
        e->lhs = std::move(lhs); e->rhs = std::move(rhs);
        lhs = std::move(e);
    }
    return lhs;
}

ExprPtr Parser::parseLogicalAnd() {
    auto lhs = parseEquality();
    while (check(TokenKind::AND_AND)) {
        std::string op = advance().lexeme;
        auto rhs = parseEquality();
        auto e = std::make_unique<Expr>();
        e->kind = Expr::Kind::BinOp; e->op = op;
        e->lhs = std::move(lhs); e->rhs = std::move(rhs);
        lhs = std::move(e);
    }
    return lhs;
}

ExprPtr Parser::parseEquality() {
    auto lhs = parseComparison();
    while (check(TokenKind::EQ_EQ) || check(TokenKind::BANG_EQ)) {
        std::string op = advance().lexeme;
        auto rhs = parseComparison();
        auto e = std::make_unique<Expr>();
        e->kind = Expr::Kind::BinOp; e->op = op;
        e->lhs = std::move(lhs); e->rhs = std::move(rhs);
        lhs = std::move(e);
    }
    return lhs;
}

ExprPtr Parser::parseComparison() {
    auto lhs = parseAddSub();
    while (check(TokenKind::LT) || check(TokenKind::GT) ||
           check(TokenKind::LT_EQ) || check(TokenKind::GT_EQ)) {
        std::string op = advance().lexeme;
        auto rhs = parseAddSub();
        auto e = std::make_unique<Expr>();
        e->kind = Expr::Kind::BinOp; e->op = op;
        e->lhs = std::move(lhs); e->rhs = std::move(rhs);
        lhs = std::move(e);
    }
    return lhs;
}

ExprPtr Parser::parseAddSub() {
    auto lhs = parseMulDiv();
    while (check(TokenKind::PLUS) || check(TokenKind::MINUS)) {
        std::string op = advance().lexeme;
        auto rhs = parseMulDiv();
        auto e = std::make_unique<Expr>();
        e->kind = Expr::Kind::BinOp; e->op = op;
        e->lhs = std::move(lhs); e->rhs = std::move(rhs);
        lhs = std::move(e);
    }
    return lhs;
}

ExprPtr Parser::parseMulDiv() {
    auto lhs = parseUnary();
    while (check(TokenKind::STAR) || check(TokenKind::SLASH) || check(TokenKind::PERCENT)) {
        std::string op = advance().lexeme;
        auto rhs = parseUnary();
        auto e = std::make_unique<Expr>();
        e->kind = Expr::Kind::BinOp; e->op = op;
        e->lhs = std::move(lhs); e->rhs = std::move(rhs);
        lhs = std::move(e);
    }
    return lhs;
}

ExprPtr Parser::parseUnary() {
    if (check(TokenKind::BANG) || check(TokenKind::MINUS) ||
        check(TokenKind::STAR) || check(TokenKind::TILDE)) {
        int eLine = peek().line;
        std::string op = advance().lexeme;
        auto operand = parseUnary();
        auto e = std::make_unique<Expr>();
        e->kind = Expr::Kind::UnaryOp; e->op = op; e->line = eLine;
        e->inner = std::move(operand);
        return e;
    }
    if (check(TokenKind::AMP)) {
        int eLine = peek().line;
        advance();
        bool mut = match(TokenKind::KW_MUT);
        auto operand = parseUnary();
        auto e = std::make_unique<Expr>();
        e->kind = mut ? Expr::Kind::BorrowMut : Expr::Kind::Borrow;
        e->line = eLine;
        e->inner = std::move(operand);
        return e;
    }
    if (check(TokenKind::AMP_MUT)) {
        int eLine = peek().line;
        advance();
        auto operand = parseUnary();
        auto e = std::make_unique<Expr>();
        e->kind = Expr::Kind::BorrowMut;
        e->line = eLine;
        e->inner = std::move(operand);
        return e;
    }
    if (check(TokenKind::KW_MOVE)) {
        int eLine = peek().line;
        advance();
        auto operand = parseUnary();
        auto e = std::make_unique<Expr>();
        e->kind = Expr::Kind::Move;
        e->line = eLine;
        e->inner = std::move(operand);
        return e;
    }
    if (check(TokenKind::KW_NEW)) {
        int eLine = peek().line;
        advance();
        auto e = std::make_unique<Expr>();
        e->kind = Expr::Kind::New;
        e->line = eLine;
        e->typeArg = std::make_unique<TypeRef>(parseType());
        expect(TokenKind::LPAREN, "Expected '(' after type in new");
        if (!check(TokenKind::RPAREN)) e->inner = parseExpr();
        expect(TokenKind::RPAREN, "Expected ')' after new expression");
        return e;
    }
    return parsePostfix();
}

// Returns true if, from the current position, what follows looks like
// a generic type-arg list followed by '(' — e.g. <int, float>(
bool Parser::looksLikeGenericCall() {
    size_t saved = pos;
    if (!check(TokenKind::LT)) return false;
    advance(); // consume '<'

    // Must see at least one type-like token
    int depth = 1;
    bool valid = true;
    while (depth > 0 && !isAtEnd() && valid) {
        if      (check(TokenKind::LT))       { depth++; advance(); }
        else if (check(TokenKind::GT))       { depth--; if (depth > 0) advance(); else advance(); }
        else if (check(TokenKind::IDENT)     ||
                 check(TokenKind::KW_INT)    || check(TokenKind::KW_FLOAT) ||
                 check(TokenKind::KW_CHAR)   || check(TokenKind::KW_BOOL)  ||
                 check(TokenKind::KW_VOID)   || check(TokenKind::COMMA)    ||
                 check(TokenKind::AMP)       || check(TokenKind::AMP_MUT)  ||
                 check(TokenKind::STAR))     { advance(); }
        else    { valid = false; }
    }

    bool result = valid && (depth == 0) && check(TokenKind::LPAREN);
    pos = saved;
    return result;
}

ExprPtr Parser::parsePostfix() {
    auto expr = parsePrimary();

    // Generic call: ident<T, U>(args)
    // Use lookahead to distinguish from comparison operators
    if (expr->kind == Expr::Kind::Ident && looksLikeGenericCall()) {
        advance(); // consume '<'
        std::vector<TypeRef> typeArgs;
        while (!check(TokenKind::GT) && !isAtEnd()) {
            typeArgs.push_back(parseType());
            if (!check(TokenKind::GT)) expect(TokenKind::COMMA, "Expected ',' between type args");
        }
        expect(TokenKind::GT, "Expected '>' after type args");
        expr->typeArgs = std::move(typeArgs);
        // The '(' will be handled by the regular call logic below
    }

    while (true) {
        if (check(TokenKind::DOT)) {
            advance();
            std::string field = expect(TokenKind::IDENT, "Expected field name").lexeme;
            auto e = std::make_unique<Expr>();
            if (check(TokenKind::LPAREN)) {
                // method call
                advance();
                e->kind = Expr::Kind::Call;
                auto callee = std::make_unique<Expr>();
                callee->kind = Expr::Kind::Member;
                callee->object = std::move(expr);
                callee->field = field;
                e->callee = std::move(callee);
                while (!check(TokenKind::RPAREN) && !isAtEnd()) {
                    e->args.push_back(parseExpr());
                    if (!check(TokenKind::RPAREN)) expect(TokenKind::COMMA, "Expected ','");
                }
                expect(TokenKind::RPAREN, "Expected ')'");
            } else {
                e->kind = Expr::Kind::Member;
                e->object = std::move(expr);
                e->field = field;
            }
            expr = std::move(e);
        } else if (check(TokenKind::LBRACKET)) {
            advance();
            auto idx = parseExpr();
            expect(TokenKind::RBRACKET, "Expected ']'");
            auto e = std::make_unique<Expr>();
            e->kind = Expr::Kind::Index;
            e->object = std::move(expr);
            e->inner = std::move(idx);
            expr = std::move(e);
        } else if (check(TokenKind::LPAREN)) {
            // function call
            advance();
            auto e = std::make_unique<Expr>();
            e->kind = Expr::Kind::Call;
            e->callee = std::move(expr);
            while (!check(TokenKind::RPAREN) && !isAtEnd()) {
                e->args.push_back(parseExpr());
                if (!check(TokenKind::RPAREN)) expect(TokenKind::COMMA, "Expected ','");
            }
            expect(TokenKind::RPAREN, "Expected ')'");
            expr = std::move(e);
        } else {
            break;
        }
    }
    return expr;
}

ExprPtr Parser::parsePrimary() {
    auto& t = peek();
    if (t.kind == TokenKind::INT_LIT) {
        advance();
        auto e = std::make_unique<Expr>();
        e->kind = Expr::Kind::IntLit;
        e->intVal = std::get<long long>(t.value);
        return e;
    }
    if (t.kind == TokenKind::FLOAT_LIT) {
        advance();
        auto e = std::make_unique<Expr>();
        e->kind = Expr::Kind::FloatLit;
        e->floatVal = std::get<double>(t.value);
        return e;
    }
    if (t.kind == TokenKind::STRING_LIT) {
        advance();
        auto e = std::make_unique<Expr>();
        e->kind = Expr::Kind::StringLit;
        e->strVal = std::get<std::string>(t.value);
        return e;
    }
    if (t.kind == TokenKind::KW_TRUE || t.kind == TokenKind::KW_FALSE) {
        advance();
        auto e = std::make_unique<Expr>();
        e->kind = Expr::Kind::BoolLit;
        e->boolVal = (t.kind == TokenKind::KW_TRUE);
        return e;
    }
    if (t.kind == TokenKind::KW_NULL) {
        advance();
        auto e = std::make_unique<Expr>();
        e->kind = Expr::Kind::NullLit;
        return e;
    }
    if (t.kind == TokenKind::IDENT) {
        advance();
        auto e = std::make_unique<Expr>();
        e->kind = Expr::Kind::Ident;
        e->name = t.lexeme;
        return e;
    }
    if (t.kind == TokenKind::LPAREN) {
        advance();
        auto e = parseExpr();
        expect(TokenKind::RPAREN, "Expected ')'");
        return e;
    }
    throw error("Expected expression");
}

} // namespace ferrum
