#include "ferrum/Lexer.h"
#include "ferrum/Parser.h"
#include "ferrum/BorrowChecker.h"
#include "ferrum/TypeChecker.h"
#include "ferrum/Codegen.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <cstdlib>   // system()
#include <filesystem>
#include <unistd.h>  // getpid()

// Shell-quote a path to prevent command injection: wraps in single quotes
// and escapes any single quotes within the path.
static std::string shellQuote(const std::string& s) {
    std::string result = "'";
    for (char c : s) {
        if (c == '\'') result += "'\\''";
        else           result += c;
    }
    result += "'";
    return result;
}

// ─── Error printing ───────────────────────────────────────────────────────────

static void printBorrowErrors(const std::vector<ferrum::BorrowError>& errors) {
    for (auto& e : errors) {
        std::string kind;
        switch (e.kind) {
            case ferrum::BorrowError::Kind::UseAfterMove:              kind = "E0382"; break;
            case ferrum::BorrowError::Kind::UseAfterFree:              kind = "E0505"; break;
            case ferrum::BorrowError::Kind::MutableBorrowWhileBorrowed:kind = "E0502"; break;
            case ferrum::BorrowError::Kind::MutateWhileBorrowed:       kind = "E0596"; break;
            case ferrum::BorrowError::Kind::BorrowOutlivesOwner:       kind = "E0597"; break;
            case ferrum::BorrowError::Kind::UnsafeOutsideUnsafeBlock:  kind = "E0133"; break;
            default:                                                    kind = "E????"; break;
        }
        std::cerr << "error[" << kind << "] line " << e.line
                  << ": " << e.message << "\n";
    }
}

static void printTypeErrors(const std::vector<ferrum::TypeChecker::TypeError>& errors) {
    for (auto& e : errors)
        std::cerr << "error[type] line " << e.line << ": " << e.message << "\n";
}

static void printCodegenErrors(const std::vector<ferrum::Codegen::CodegenError>& errors) {
    for (auto& e : errors)
        std::cerr << "error[codegen] line " << e.line << ": " << e.message << "\n";
}

// ─── RAII guard to delete temp files unconditionally ─────────────────────────

struct TempFileGuard {
    std::vector<std::string> paths;
    ~TempFileGuard() {
        for (auto& p : paths) {
            std::error_code ec;
            std::filesystem::remove(p, ec);
        }
    }
};

// ─── Compile a single file ────────────────────────────────────────────────────

// Maximum source file size accepted (10 MiB).
static constexpr std::streamoff MAX_SOURCE_BYTES = 10 * 1024 * 1024;

static int runFile(const std::string& path, bool emitIR, const std::string& outputBin) {
    // Reject paths containing null bytes — they can trick downstream C APIs.
    if (path.find('\0') != std::string::npos) {
        std::cerr << "error: invalid input path\n";
        return 1;
    }

    // Enforce .fe extension so the compiler can't be silently fed arbitrary files.
    if (std::filesystem::path(path).extension() != ".fe") {
        std::cerr << "error: Ferrum-language source files must have the .fe extension\n";
        return 1;
    }

    // Read source with a size cap to prevent memory exhaustion.
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) { std::cerr << "Cannot open file: " << path << "\n"; return 1; }
    std::streamoff fileSize = f.tellg();
    if (fileSize > MAX_SOURCE_BYTES) {
        std::cerr << "error: source file too large (max 10 MiB)\n";
        return 1;
    }
    f.seekg(0, std::ios::beg);
    std::ostringstream buf; buf << f.rdbuf();
    std::string src = buf.str();

    bool hasErrors = false;

    try {
        // ── 1. Lex ────────────────────────────────────────────────────────────
        ferrum::Lexer lexer(src, path);
        auto tokens = lexer.tokenize();

        // ── 2. Parse ──────────────────────────────────────────────────────────
        ferrum::Parser parser(tokens);
        auto program = parser.parse();

        // ── 3. Type Check (Sema) ──────────────────────────────────────────────
        ferrum::TypeChecker tc;
        tc.check(program);
        if (!tc.errors.empty()) {
            printTypeErrors(tc.errors);
            std::cerr << tc.errors.size() << " type error(s).\n";
            hasErrors = true;
        }

        // ── 4. Borrow Check ───────────────────────────────────────────────────
        ferrum::BorrowChecker bc;
        bc.check(program);
        if (!bc.errors.empty()) {
            printBorrowErrors(bc.errors);
            std::cerr << bc.errors.size() << " borrow error(s).\n";
            hasErrors = true;
        }

        if (hasErrors) {
            std::cerr << "\nCompilation failed.\n";
            return 1;
        }

        // ── 5. Codegen (LLVM IR) ──────────────────────────────────────────────
        std::string moduleName = std::filesystem::path(path).stem().string();
        ferrum::Codegen cg(moduleName, tc);
        cg.generate(program);

        if (!cg.errors.empty()) {
            printCodegenErrors(cg.errors);
            std::cerr << cg.errors.size() << " codegen error(s).\n";
            // Don't abort — some are warnings (e.g. generic codegen not yet supported)
        }

        if (emitIR) {
            std::cout << cg.getIR();
            return 0;
        }

        // Write IR to a temp file (include PID to avoid collisions in shared environments)
        std::string pid      = std::to_string(getpid());
        std::string irPath   = "/tmp/ferrum_" + moduleName + "_" + pid + ".ll";
        std::string objPath  = "/tmp/ferrum_" + moduleName + "_" + pid + ".o";

        // Temp files are deleted on scope exit regardless of success or failure.
        TempFileGuard tempGuard;
        tempGuard.paths.push_back(irPath);
        tempGuard.paths.push_back(objPath);

        if (!cg.writeIR(irPath)) {
            std::cerr << "Failed to write IR to " << irPath << "\n";
            return 1;
        }

        // ── 6. Compile IR → binary using llc + gcc ────────────────────────────
        std::string outPath = outputBin.empty() ? ("./" + moduleName) : outputBin;

        // Try clang first, fall back to llc + gcc
        // Paths are shell-quoted to prevent command injection
        std::string clangCmd = "clang " + shellQuote(irPath) + " -o " + shellQuote(outPath) + " -lm 2>/dev/null";
        std::string llcCmd   = "llc -filetype=obj -relocation-model=pic " + shellQuote(irPath) + " -o " + shellQuote(objPath) + " 2>&1";
        std::string gccCmd   = "gcc -fPIE " + shellQuote(objPath) + " -o " + shellQuote(outPath) + " -lm 2>&1";

        int ret = std::system(clangCmd.c_str());
        if (ret != 0) {
            // clang not found — use llc + gcc
            ret = std::system(llcCmd.c_str());
            if (ret != 0) {
                std::cerr << "llc failed with exit code " << ret << "\n";
                return 1;
            }
            ret = std::system(gccCmd.c_str());
            if (ret != 0) {
                std::cerr << "gcc failed with exit code " << ret << "\n";
                return 1;
            }
        }

        std::cout << "✓ " << path << " → " << outPath << "\n";
        return 0;

    } catch (const ferrum::ParseError& pe) {
        std::cerr << "parse error: " << pe.what() << "\n";
        return 1;
    }
}

// ─── REPL ─────────────────────────────────────────────────────────────────────

static void runRepl() {
    std::cout << "Ferrum-language REPL. Type code, end with a blank line.\n\n";
    while (true) {
        std::string line, src;
        std::cout << "fe> ";
        while (std::getline(std::cin, line)) {
            if (line.empty()) break;
            src += line + "\n";
            std::cout << "    ";
        }
        if (src.empty()) break;
        try {
            ferrum::Lexer lexer(src, "<repl>");
            auto tokens = lexer.tokenize();
            ferrum::Parser parser(tokens);
            auto program = parser.parse();

            ferrum::TypeChecker tc;
            tc.check(program);
            if (!tc.errors.empty()) { printTypeErrors(tc.errors); continue; }

            ferrum::BorrowChecker bc;
            bc.check(program);
            if (!bc.errors.empty()) { printBorrowErrors(bc.errors); continue; }

            std::cout << "✓ OK (sema + borrow check passed)\n";
        } catch (const ferrum::ParseError& e) {
            std::cerr << "parse error: " << e.what() << "\n";
        }
    }
}

// ─── main ─────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    std::cout << "Ferrum-language Compiler v0.3\n";
    std::cout << "Syntax: C | Safety: compile-time checked | Ecosystem: C++\n";
    std::cout << "─────────────────────────────────────────\n";

    if (argc < 2) {
        runRepl();
        return 0;
    }

    std::string inputFile;
    std::string outputBin;
    bool emitIR = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--emit-ir" || arg == "--emit-llvm") {
            emitIR = true;
        } else if ((arg == "-o") && i+1 < argc) {
            outputBin = argv[++i];
        } else if (arg[0] != '-') {
            inputFile = arg;
        }
    }

    if (inputFile.empty()) {
        std::cerr << "Usage: ferrumc <file.fe> [--emit-ir] [-o output]\n";
        return 1;
    }

    // Reject null bytes in user-supplied paths before they reach C APIs.
    if (inputFile.find('\0') != std::string::npos ||
        (!outputBin.empty() && outputBin.find('\0') != std::string::npos)) {
        std::cerr << "error: invalid path (contains null byte)\n";
        return 1;
    }

    return runFile(inputFile, emitIR, outputBin);
}
