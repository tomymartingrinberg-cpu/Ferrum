cmake_minimum_required(VERSION 3.16)
project(ferrum-language VERSION 0.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ─── LLVM ─────────────────────────────────────────────────────────────────────
find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})
llvm_map_components_to_libnames(llvm_libs core support irreader)

# ─── Core library ─────────────────────────────────────────────────────────────
include_directories(include)

add_library(ferrum_core
    src/lexer/Lexer.cpp
    src/parser/Parser.cpp
    src/borrow/BorrowChecker.cpp
    src/sema/TypeChecker.cpp
    src/codegen/Codegen.cpp
)
target_include_directories(ferrum_core PUBLIC include ${LLVM_INCLUDE_DIRS})
target_link_libraries(ferrum_core ${llvm_libs})

# ─── Compiler driver ──────────────────────────────────────────────────────────
add_executable(ferrumc src/driver/main.cpp)
target_link_libraries(ferrumc ferrum_core)

# ─── Tests ────────────────────────────────────────────────────────────────────
enable_testing()
add_subdirectory(tests)
