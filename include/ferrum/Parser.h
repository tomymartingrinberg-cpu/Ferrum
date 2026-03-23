#pragma once
#include "ferrum/Token.h"
#include "ferrum/AST.h"
#include <vector>
#include <stdexcept>

namespace ferrum {

class ParseError : public std::runtime_error {
public:
    int line, col;
    ParseError(const std::string& msg, int l, int c)
        : std::runtime_error(msg), line(l), col(c) {}
};

class Parser {
public:
    explicit Parser(std::vector<Token> tokens);
    Program parse();

private:
    std::vector<Token> tokens;
    size_t pos = 0;

    // Recursion depth guard: prevents stack overflow on deeply nested input.
    int nestingDepth = 0;
    static constexpr int MAX_NESTING_DEPTH = 512;

    // ── Token helpers ──────────────────────────────────────────
    Token& peek(int offset = 0);
    Token& advance();
    bool check(TokenKind k) const;
    bool match(TokenKind k);
    Token expect(TokenKind k, const std::string& msg);
    bool isAtEnd() const;

    // ── Top-level ──────────────────────────────────────────────
    DeclPtr parseDecl();
    DeclPtr parseFunctionDecl();
    DeclPtr parseStructDecl();
    DeclPtr parseExternBlock();
    DeclPtr parseImportDecl();

    // ── Types ──────────────────────────────────────────────────
    TypeRef parseType();

    // ── Statements ─────────────────────────────────────────────
    StmtPtr parseStmt();
    StmtPtr parseBlock();
    StmtPtr parseVarDecl();
    StmtPtr parseIf();
    StmtPtr parseWhile();
    StmtPtr parseFor();
    StmtPtr parseReturn();
    StmtPtr parseUnsafeBlock();

    // ── Expressions ────────────────────────────────────────────
    bool looksLikeGenericCall();
    ExprPtr parseExpr();
    ExprPtr parseAssign();
    ExprPtr parseLogicalOr();
    ExprPtr parseLogicalAnd();
    ExprPtr parseEquality();
    ExprPtr parseComparison();
    ExprPtr parseAddSub();
    ExprPtr parseMulDiv();
    ExprPtr parseUnary();
    ExprPtr parsePostfix();
    ExprPtr parsePrimary();

    ParseError error(const std::string& msg);
};

} // namespace ferrum
