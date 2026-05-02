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
#include <cstdio>    // getpid / _getpid

#ifdef _WIN32
#  include <process.h>
#  define GET_PID() _getpid()
#  define DEV_NULL  "NUL"
#else
#  include <unistd.h>
#  define GET_PID() getpid()
#  define DEV_NULL  "/dev/null"
#endif

// Sanitize a module name for use in temp file paths: replace every character
// that is not alphanumeric or '_' with '_' to prevent path traversal.
static std::string sanitizeModuleName(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (unsigned char c : name)
        out += (std::isalnum(c) || c == '_') ? (char)c : '_';
    if (out.empty() || out == "_") out = "module";
    return out;
}

// Reject writes to sensitive system directories.
static bool isSafeOutputPath(const std::string& p) {
    static const char* const dangerous[] = {
        "/proc/", "/sys/", "/dev/", "/etc/", "/boot/", "/run/", "/sbin/"
    };
    for (auto prefix : dangerous)
        if (p.rfind(prefix, 0) == 0) return false;
    return true;
}

// Shell-quote a path to prevent command injection.
static std::string shellQuote(const std::string& s) {
#ifdef _WIN32
    // On Windows cmd.exe, wrap in double quotes and escape internal double quotes.
    std::string result = "\"";
    for (char c : s) {
        if (c == '"') result += "\\\"";
        else          result += c;
    }
    result += "\"";
    return result;
#else
    std::string result = "'";
    for (char c : s) {
        if (c == '\'') result += "'\\''";
        else           result += c;
    }
    result += "'";
    return result;
#endif
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

static constexpr std::streamoff MAX_SOURCE_BYTES = 10 * 1024 * 1024;

static int runFile(const std::string& path, bool emitIR, const std::string& outputBin) {
    if (path.find('\0') != std::string::npos) {
        std::cerr << "error: invalid input path\n";
        return 1;
    }

    if (std::filesystem::path(path).extension() != ".fe") {
        std::cerr << "error: Ferrum-language source files must have the .fe extension\n";
        return 1;
    }

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
        ferrum::Lexer lexer(src, path);
        auto tokens = lexer.tokenize();

        ferrum::Parser parser(tokens);
        auto program = parser.parse();

        ferrum::TypeChecker tc;
        tc.check(program);
        if (!tc.errors.empty()) {
            printTypeErrors(tc.errors);
            std::cerr << tc.errors.size() << " type error(s).\n";
            hasErrors = true;
        }

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

        std::string moduleName = std::filesystem::path(path).stem().string();
        ferrum::Codegen cg(moduleName, tc);
        cg.generate(program);

        if (!cg.errors.empty()) {
            printCodegenErrors(cg.errors);
            std::cerr << cg.errors.size() << " codegen error(s).\n";
        }

        if (emitIR) {
            std::cout << cg.getIR();
            return 0;
        }

        // Use std::filesystem::temp_directory_path() for cross-platform temp dir
        // (returns C:\Users\...\AppData\Local\Temp on Windows, /tmp on Unix).
        std::string pid      = std::to_string(GET_PID());
        std::string safeBase = sanitizeModuleName(moduleName);
        auto        tmpDir   = std::filesystem::temp_directory_path();
        std::string irPath   = (tmpDir / ("ferrum_" + safeBase + "_" + pid + ".ll")).string();
        std::string objPath  = (tmpDir / ("ferrum_" + safeBase + "_" + pid + ".o")).string();

        TempFileGuard tempGuard;
        tempGuard.paths.push_back(irPath);
        tempGuard.paths.push_back(objPath);

        if (!cg.writeIR(irPath)) {
            std::cerr << "Failed to write IR to " << irPath << "\n";
            return 1;
        }

        {
            std::error_code ec;
            std::filesystem::permissions(irPath,
                std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                std::filesystem::perm_options::replace, ec);
        }

        std::string outPath = outputBin.empty() ?
#ifdef _WIN32
            ("./" + moduleName + ".exe")
#else
            ("./" + moduleName)
#endif
            : outputBin;

        if (!isSafeOutputPath(std::filesystem::absolute(outPath).string())) {
            std::cerr << "error: output path '" << outPath << "' is not allowed\n";
            return 1;
        }

        // Try clang first, fall back to llc + gcc.
        // Use platform-correct null device for stderr suppression.
        std::string clangCmd = "clang " + shellQuote(irPath) + " -o " + shellQuote(outPath) + " -lm 2>" DEV_NULL;
        std::string llcCmd   = "llc -filetype=obj -relocation-model=pic " + shellQuote(irPath) + " -o " + shellQuote(objPath) + " 2>&1";
        std::string gccCmd   = "gcc -fPIE " + shellQuote(objPath) + " -o " + shellQuote(outPath) + " -lm 2>&1";

        int ret = std::system(clangCmd.c_str());
        if (ret != 0) {
            ret = std::system(llcCmd.c_str());
            if (ret != 0) {
                std::cerr << "error: 'llc' not found. Install LLVM or clang to compile Ferrum programs.\n"
                          << "  On Windows: winget install LLVM.LLVM\n"
                          << "  On Ubuntu:  sudo apt install llvm clang\n"
                          << "  On macOS:   brew install llvm\n";
                return 1;
            }
            ret = std::system(gccCmd.c_str());
            if (ret != 0) {
                std::cerr << "error: 'gcc' not found. Install GCC/MinGW to link Ferrum programs.\n"
                          << "  On Windows: winget install GnuWin32.GCC  or  choco install mingw\n"
                          << "  On Ubuntu:  sudo apt install gcc\n"
                          << "  On macOS:   brew install gcc\n";
                return 1;
            }
        }

        std::cout << "Compiled: " << path << " -> " << outPath << "\n";
        return 0;

    } catch (const ferrum::ParseError& pe) {
        std::cerr << "parse error: " << pe.what() << "\n";
        return 1;
    }
}

// ─── REPL ─────────────────────────────────────────────────────────────────────

static void runRepl() {
    std::cout << "Ferrum-language REPL v0.3 — type code, end input with a blank line, Ctrl+D/Ctrl+Z to exit.\n\n";
    while (true) {
        std::string line, src;
        std::cout << "fe> " << std::flush;
        while (std::getline(std::cin, line)) {
            if (line.empty()) break;
            src += line + "\n";
            std::cout << "... " << std::flush;
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

            std::cout << "OK (sema + borrow check passed)\n";
        } catch (const ferrum::ParseError& e) {
            std::cerr << "parse error: " << e.what() << "\n";
        }
    }
}

// ─── main ─────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    std::cout << "Ferrum-language Compiler v0.3\n";
    std::cout << "Syntax: C | Safety: compile-time checked | Ecosystem: C++\n";
    std::cout << "-----------------------------------------\n";

    if (argc < 2) {
        std::cout << "Usage: ferrumc <file.fe> [--emit-ir] [-o output]\n"
                  << "       ferrumc              (start REPL)\n\n";
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

    if (inputFile.find('\0') != std::string::npos ||
        (!outputBin.empty() && outputBin.find('\0') != std::string::npos)) {
        std::cerr << "error: invalid path (contains null byte)\n";
        return 1;
    }

    return runFile(inputFile, emitIR, outputBin);
}
