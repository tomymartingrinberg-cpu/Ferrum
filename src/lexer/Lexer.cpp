#include "ferrum/Lexer.h"
#include <stdexcept>
#include <sstream>

namespace ferrum {

const std::unordered_map<std::string, TokenKind> Lexer::keywords = {
    {"int",    TokenKind::KW_INT},
    {"float",  TokenKind::KW_FLOAT},
    {"char",   TokenKind::KW_CHAR},
    {"void",   TokenKind::KW_VOID},
    {"bool",   TokenKind::KW_BOOL},
    {"if",     TokenKind::KW_IF},
    {"else",   TokenKind::KW_ELSE},
    {"while",  TokenKind::KW_WHILE},
    {"for",    TokenKind::KW_FOR},
    {"return", TokenKind::KW_RETURN},
    {"struct", TokenKind::KW_STRUCT},
    {"enum",   TokenKind::KW_ENUM},
    {"import", TokenKind::KW_IMPORT},
    {"extern", TokenKind::KW_EXTERN},
    {"new",    TokenKind::KW_NEW},
    {"move",   TokenKind::KW_MOVE},
    {"unsafe", TokenKind::KW_UNSAFE},
    {"mut",    TokenKind::KW_MUT},
    {"true",   TokenKind::KW_TRUE},
    {"false",  TokenKind::KW_FALSE},
    {"null",   TokenKind::KW_NULL},
};

Lexer::Lexer(std::string source, std::string fname)
    : src(std::move(source)), filename(std::move(fname)) {}

char Lexer::peek(int offset) const {
    size_t idx = pos + offset;
    if (idx >= src.size()) return '\0';
    return src[idx];
}

char Lexer::advance() {
    char c = src[pos++];
    if (c == '\n') { line++; col = 1; }
    else col++;
    return c;
}

bool Lexer::match(char expected) {
    if (isAtEnd() || src[pos] != expected) return false;
    advance();
    return true;
}

void Lexer::skipWhitespaceAndComments() {
    while (!isAtEnd()) {
        char c = peek();
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance();
        } else if (c == '/' && peek(1) == '/') {
            // Line comment
            while (!isAtEnd() && peek() != '\n') advance();
        } else if (c == '/' && peek(1) == '*') {
            // Block comment
            advance(); advance();
            while (!isAtEnd() && !(peek() == '*' && peek(1) == '/')) advance();
            if (!isAtEnd()) { advance(); advance(); } // consume */
        } else {
            break;
        }
    }
}

Token Lexer::makeToken(TokenKind kind, const std::string& lex) const {
    return Token(kind, lex, line, col);
}

Token Lexer::lexNumber() {
    int startLine = line, startCol = col;
    size_t start = pos - 1;
    bool isFloat = false;

    while (!isAtEnd() && isDigit(peek())) advance();
    if (!isAtEnd() && peek() == '.' && isDigit(peek(1))) {
        isFloat = true;
        advance(); // consume '.'
        while (!isAtEnd() && isDigit(peek())) advance();
    }

    std::string lex = src.substr(start, pos - start);
    Token tok(isFloat ? TokenKind::FLOAT_LIT : TokenKind::INT_LIT,
              lex, startLine, startCol);
    try {
        if (isFloat)
            tok.value = std::stod(lex);
        else
            tok.value = std::stoll(lex);
    } catch (const std::out_of_range&) {
        return Token(TokenKind::ERROR,
                     "numeric literal out of range: " + lex, startLine, startCol);
    } catch (const std::invalid_argument&) {
        return Token(TokenKind::ERROR,
                     "invalid numeric literal: " + lex, startLine, startCol);
    }
    return tok;
}

Token Lexer::lexString() {
    int startLine = line, startCol = col;
    std::string result;
    while (!isAtEnd() && peek() != '"') {
        char c = advance();
        if (c == '\\') {
            char esc = advance();
            switch (esc) {
                case 'n': result += '\n'; break;
                case 't': result += '\t'; break;
                case '\\': result += '\\'; break;
                case '"': result += '"'; break;
                default: result += esc;
            }
        } else {
            result += c;
        }
    }
    if (isAtEnd()) {
        return Token(TokenKind::ERROR, "unterminated string", startLine, startCol);
    }
    advance(); // closing "
    Token tok(TokenKind::STRING_LIT, "\"" + result + "\"", startLine, startCol);
    tok.value = result;
    return tok;
}

Token Lexer::lexChar() {
    int startLine = line, startCol = col;
    if (isAtEnd())
        return Token(TokenKind::ERROR, "unterminated char literal", startLine, startCol);
    char c = advance();
    if (c == '\\') {
        if (isAtEnd())
            return Token(TokenKind::ERROR, "unterminated char literal", startLine, startCol);
        char esc = advance();
        switch (esc) {
            case 'n':  c = '\n'; break;
            case 't':  c = '\t'; break;
            case 'r':  c = '\r'; break;
            case '0':  c = '\0'; break;
            case '\\': c = '\\'; break;
            case '\'': c = '\''; break;
            default:   c = esc;  break;
        }
    }
    if (peek() != '\'')
        return Token(TokenKind::ERROR, "unterminated char literal", startLine, startCol);
    advance(); // closing '
    Token tok(TokenKind::CHAR_LIT, std::string(1, c), startLine, startCol);
    tok.value = (long long)c;
    return tok;
}

Token Lexer::lexLifetime(int startLine, int startCol) {
    // Already consumed the '\'' — now read the identifier
    size_t start = pos;
    while (!isAtEnd() && isAlphaNum(peek())) advance();
    std::string name = src.substr(start, pos - start);
    Token tok(TokenKind::LIFETIME, "'" + name, startLine, startCol);
    tok.value = name;  // store the name without the '
    return tok;
}

Token Lexer::lexIdent() {
    size_t start = pos - 1;
    int startLine = line, startCol = col;
    while (!isAtEnd() && isAlphaNum(peek())) advance();
    std::string lex = src.substr(start, pos - start);

    // Check for &mut
    auto it = keywords.find(lex);
    TokenKind kind = (it != keywords.end()) ? it->second : TokenKind::IDENT;
    return Token(kind, lex, startLine, startCol);
}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> tokens;

    // Reject source containing null bytes — they can bypass checks in C APIs
    // and produce confusing parse errors deep in the pipeline.
    if (src.find('\0') != std::string::npos) {
        tokens.push_back(Token(TokenKind::ERROR,
            "source file contains a null byte", 1, 1));
        tokens.push_back(Token(TokenKind::EOF_TOK, "", 1, 1));
        return tokens;
    }

    // Cap token count to prevent memory exhaustion from pathological input.
    static constexpr size_t MAX_TOKENS = 1'000'000;

    while (true) {
        skipWhitespaceAndComments();
        if (isAtEnd()) break;

        if (tokens.size() >= MAX_TOKENS) {
            tokens.push_back(Token(TokenKind::ERROR,
                "source file exceeds token limit (" +
                std::to_string(MAX_TOKENS) + " tokens)", line, col));
            break;
        }

        int startLine = line, startCol = col;
        char c = advance();

        switch (c) {
            case '(': tokens.push_back(makeToken(TokenKind::LPAREN,   "(")); break;
            case ')': tokens.push_back(makeToken(TokenKind::RPAREN,   ")")); break;
            case '{': tokens.push_back(makeToken(TokenKind::LBRACE,   "{")); break;
            case '}': tokens.push_back(makeToken(TokenKind::RBRACE,   "}")); break;
            case '[': tokens.push_back(makeToken(TokenKind::LBRACKET, "[")); break;
            case ']': tokens.push_back(makeToken(TokenKind::RBRACKET, "]")); break;
            case ';': tokens.push_back(makeToken(TokenKind::SEMICOLON,";")); break;
            case ',': tokens.push_back(makeToken(TokenKind::COMMA,    ",")); break;
            case '.': tokens.push_back(makeToken(TokenKind::DOT,      ".")); break;
            case ':':
                if (match(':')) tokens.push_back(Token(TokenKind::DOUBLE_COLON, "::", startLine, startCol));
                else            tokens.push_back(Token(TokenKind::COLON,        ":",  startLine, startCol));
                break;
            case '~': tokens.push_back(makeToken(TokenKind::TILDE,    "~")); break;
            case '^': tokens.push_back(makeToken(TokenKind::CARET,    "^")); break;
            case '%': tokens.push_back(makeToken(TokenKind::PERCENT,  "%")); break;
            case '+': { bool eq = match('='); tokens.push_back(Token(eq ? TokenKind::PLUS_EQ  : TokenKind::PLUS,  eq ? "+=" : "+",  startLine, startCol)); break; }
            case '-': {
                if      (match('>')) tokens.push_back(Token(TokenKind::ARROW,    "->", startLine, startCol));
                else if (match('=')) tokens.push_back(Token(TokenKind::MINUS_EQ, "-=", startLine, startCol));
                else                 tokens.push_back(Token(TokenKind::MINUS,    "-",  startLine, startCol));
                break;
            }
            case '*': { bool eq = match('='); tokens.push_back(Token(eq ? TokenKind::STAR_EQ  : TokenKind::STAR,  eq ? "*=" : "*",  startLine, startCol)); break; }
            case '/': { bool eq = match('='); tokens.push_back(Token(eq ? TokenKind::SLASH_EQ : TokenKind::SLASH, eq ? "/=" : "/",  startLine, startCol)); break; }
            case '!': { bool eq = match('='); tokens.push_back(Token(eq ? TokenKind::BANG_EQ  : TokenKind::BANG,  eq ? "!=" : "!",  startLine, startCol)); break; }
            case '=': { bool eq = match('='); tokens.push_back(Token(eq ? TokenKind::EQ_EQ    : TokenKind::EQ,    eq ? "==" : "=",  startLine, startCol)); break; }
            case '<': { bool eq = match('='); tokens.push_back(Token(eq ? TokenKind::LT_EQ    : TokenKind::LT,    eq ? "<=" : "<",  startLine, startCol)); break; }
            case '>': { bool eq = match('='); tokens.push_back(Token(eq ? TokenKind::GT_EQ    : TokenKind::GT,    eq ? ">=" : ">",  startLine, startCol)); break; }
            case '&':
                if (peek() == 'm' && src.substr(pos, 3) == "mut" &&
                    (pos + 3 >= src.size() || !isAlphaNum(src[pos + 3]))) {
                    pos += 3; col += 3;
                    tokens.push_back(Token(TokenKind::AMP_MUT, "&mut", startLine, startCol));
                } else if (match('&')) {
                    tokens.push_back(Token(TokenKind::AND_AND, "&&", startLine, startCol));
                } else {
                    tokens.push_back(Token(TokenKind::AMP, "&", startLine, startCol));
                }
                break;
            case '|': { bool pp = match('|'); tokens.push_back(Token(pp ? TokenKind::PIPE_PIPE : TokenKind::PIPE, pp ? "||" : "|", startLine, startCol)); break; }
            case '"': tokens.push_back(lexString()); break;
            case '\'': {
                // Lifetime 'a or char literal 'x'
                // If next is alpha and the char after that is not '\'', it's a lifetime
                if (isAlpha(peek()) && peek(1) != '\'') {
                    tokens.push_back(lexLifetime(startLine, startCol));
                } else {
                    tokens.push_back(lexChar());
                }
                break;
            }
            case '#': {
                // C preprocessor directive — skip whitespace (not newline) after #
                while (!isAtEnd() && (peek() == ' ' || peek() == '\t')) advance();
                // Read directive name
                size_t dstart = pos;
                while (!isAtEnd() && isAlpha(peek())) advance();
                std::string directive = src.substr(dstart, pos - dstart);

                if (directive == "include") {
                    // Skip whitespace
                    while (!isAtEnd() && (peek() == ' ' || peek() == '\t')) advance();
                    // Emit KW_IMPORT so the parser handles it identically to 'import'
                    tokens.push_back(Token(TokenKind::KW_IMPORT, "import", startLine, startCol));
                    if (!isAtEnd() && peek() == '<') {
                        advance(); // consume '<'
                        tokens.push_back(Token(TokenKind::LT, "<", line, col));
                        // Read full path up to '>' as a single IDENT token
                        size_t pstart = pos;
                        while (!isAtEnd() && peek() != '>' && peek() != '\n') advance();
                        std::string path = src.substr(pstart, pos - pstart);
                        tokens.push_back(Token(TokenKind::IDENT, path, line, col));
                        if (!isAtEnd() && peek() == '>') advance();
                        tokens.push_back(Token(TokenKind::GT, ">", line, col));
                    } else if (!isAtEnd() && peek() == '"') {
                        advance(); // consume opening "
                        size_t pstart = pos;
                        while (!isAtEnd() && peek() != '"' && peek() != '\n') advance();
                        std::string path = src.substr(pstart, pos - pstart);
                        if (!isAtEnd()) advance(); // consume closing "
                        Token strTok(TokenKind::STRING_LIT, "\"" + path + "\"", line, col);
                        strTok.value = path;
                        tokens.push_back(strTok);
                    }
                    // Auto-insert semicolon (C's #include has no ';')
                    tokens.push_back(Token(TokenKind::SEMICOLON, ";", line, col));
                } else {
                    // #define, #pragma, #ifndef, #endif, etc. — skip the whole line
                    while (!isAtEnd() && peek() != '\n') advance();
                }
                break;
            }
            default:
                if (isDigit(c)) tokens.push_back(lexNumber());
                else if (isAlpha(c)) tokens.push_back(lexIdent());
                else tokens.push_back(Token(TokenKind::ERROR,
                                            std::string(1, c), startLine, startCol));
        }
    }

    tokens.push_back(Token(TokenKind::EOF_TOK, "", line, col));
    return tokens;
}

} // namespace ferrum
