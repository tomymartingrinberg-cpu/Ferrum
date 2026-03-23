cmake_minimum_required(VERSION 3.16)
project(ferrum-language VERSION 0.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ─── LLVM ─────────────────────────────────────────────────────────────────────
# Try cmake config first (Linux, macOS, FreeBSD, Windows with dev package).
# Fall back to llvm-config for environments where only LLVM binaries are
# installed without cmake dev files (e.g. Windows CI runners).
find_package(LLVM CONFIG QUIET)
if(LLVM_FOUND)
    message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION} via CMake config")
    message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")
    include_directories(${LLVM_INCLUDE_DIRS})
    add_definitions(${LLVM_DEFINITIONS})
    llvm_map_components_to_libnames(llvm_libs core support irreader)
else()
    message(STATUS "LLVMConfig.cmake not found — falling back to llvm-config")
    find_program(LLVM_CONFIG_EXE
        NAMES llvm-config-18 llvm-config
        HINTS ENV LLVM_PATH PATH_SUFFIXES bin
        REQUIRED)
    execute_process(COMMAND ${LLVM_CONFIG_EXE} --version
        OUTPUT_VARIABLE LLVM_PACKAGE_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND ${LLVM_CONFIG_EXE} --includedir
        OUTPUT_VARIABLE LLVM_INCLUDE_DIRS   OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND ${LLVM_CONFIG_EXE} --libdir
        OUTPUT_VARIABLE LLVM_LIBRARY_DIR    OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND ${LLVM_CONFIG_EXE} --libs core support irreader
        OUTPUT_VARIABLE _llvm_libs_raw      OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND ${LLVM_CONFIG_EXE} --system-libs
        OUTPUT_VARIABLE _llvm_syslibs_raw   OUTPUT_STRIP_TRAILING_WHITESPACE)
    message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION} via llvm-config: ${LLVM_CONFIG_EXE}")
    include_directories(${LLVM_INCLUDE_DIRS})
    link_directories(${LLVM_LIBRARY_DIR})
    separate_arguments(llvm_libs    UNIX_COMMAND "${_llvm_libs_raw}")
    separate_arguments(_llvm_syslibs UNIX_COMMAND "${_llvm_syslibs_raw}")
    list(APPEND llvm_libs ${_llvm_syslibs})
endif()

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
