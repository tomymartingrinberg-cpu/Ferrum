# Ferrum Language Specification v0.2

> **Syntax: C | Safety: compile-time checked | Ecosystem: C++**

Ferrum is a systems programming language that combines the familiar syntax of C with compile-time memory safety checks, while maintaining full interoperability with the C and C++ ecosystem.

> Also available in: [Español](SPEC.es.md)

---

## Table of Contents

1. [Design Philosophy](#1-design-philosophy)
2. [Relationship with C](#2-relationship-with-c)
3. [Variables and Types](#3-variables-and-types)
4. [Pointers and References](#4-pointers-and-references)
5. [Ownership](#5-ownership)
6. [Borrow Checker](#6-borrow-checker)
7. [Lifetimes](#7-lifetimes)
8. [Unsafe](#8-unsafe)
9. [Generics](#9-generics)
10. [Structs](#10-structs)
11. [Functions](#11-functions)
12. [Control Flow](#12-control-flow)
13. [Imports and C/C++ Interoperability](#13-imports-and-cc-interoperability)
14. [Compiler Pipeline](#14-compiler-pipeline)
15. [Compiler Errors](#15-compiler-errors)
16. [C vs Ferrum Comparison](#16-c-vs-ferrum-comparison)

---

## 1. Design Philosophy

Ferrum starts from a simple question: **what if C had a built-in borrow checker?**

### Core principles

| # | Principle |
|---|-----------|
| 1 | **If it compiles, it is memory-safe** — no undefined behavior from ownership violations |
| 2 | **Valid C is valid Ferrum** — existing C code works without modification |
| 3 | **Zero runtime overhead** — all checks happen at compile time |
| 4 | **No garbage collector** — the programmer controls memory; the compiler frees it |
| 5 | **C++ is a first-class citizen** — C++ headers, STL, and ABIs work directly |

### What Ferrum adds to C

```
Pure C                          Ferrum
──────────────────────────────────────────────────────────────
int* p = malloc(sizeof(int));   int* p = new int(42);
*p = 42;                        // free() inserted automatically
free(p);                        // when leaving scope

// No protection:               // Protected at compile time:
int* q = p;                     int* q = move(p);
free(p);  // double free!       free(p);  // ERROR: p was moved
*p = 1;   // use-after-free!    *p = 1;   // ERROR: use of moved value
```

---

## 2. Relationship with C

### C is a subset of Ferrum

Every valid C program is also a valid Ferrum program. The only syntactic difference is that C uses `#include` and Ferrum uses `import`, but **both forms are accepted**:

```c
// C style — works in Ferrum
#include <stdio.h>
#include <stdlib.h>

// Ferrum style — exact equivalent
import <stdio.h>;
import <stdlib.h>;
```

### What Ferrum does with C code

Ferrum does not reject C code — it **analyzes it and adds safety on top**:

```c
// This is pure C — Ferrum accepts it
int factorial(int n) {
    if (n <= 1) return 1;
    return n * factorial(n - 1);
}

int main() {
    int i;
    for (i = 1; i <= 7; i = i + 1) {
        printf("%d! = %d\n", i, factorial(i));
    }
    return 0;
}
```

Ferrum runs the **borrow checker and type checker** on top of this code. If there are memory violations, it catches them. If not, it compiles normally.

### What Ferrum catches that C does not

| Bug | C | Ferrum |
|-----|---|--------|
| Use-after-free | Runtime crash | `error[E0505]` at compile time |
| Double free | Runtime crash | `error[E0382]` at compile time |
| Use-after-move | Undefined behavior | `error[E0382]` at compile time |
| Dangling borrow | Invalid memory access | `error[E0597]` at compile time |
| Mutation with active borrow | Runtime data race | `error[E0596]` at compile time |
| Raw pointer outside unsafe | No protection | `error[E0133]` at compile time |

---

## 3. Variables and Types

### Primitive types

| Ferrum type | C equivalent | Size  | Description |
|-------------|--------------|-------|-------------|
| `int`       | `int`        | 32-bit | Signed integer |
| `float`     | `double`     | 64-bit | Floating point |
| `char`      | `char`       | 8-bit  | Character / byte |
| `bool`      | `_Bool`      | 1-bit  | Boolean |
| `void`      | `void`       | —      | No value |

### Variable declarations

```c
// Explicit type
int x = 42;
float pi = 3.14;
bool active = true;
char letter = 'A';

// Uninitialized (undefined value — same as C)
int counter;
```

### Stack vs heap variables

```c
int x = 10;           // stack — freed automatically when leaving scope
int* p = new int(10); // heap  — Ferrum inserts free() when leaving scope
```

---

## 4. Pointers and References

Ferrum has four pointer types, each with distinct semantics:

### Pointer table

| Syntax          | Name             | Semantics                                          |
|-----------------|------------------|----------------------------------------------------|
| `int* p`        | Owning pointer   | p is the owner — freed when p goes out of scope   |
| `int& p`        | Immutable borrow | read-only — multiple simultaneous borrows OK       |
| `int&mut p`     | Mutable borrow   | read+write — only one at a time                    |
| `int* unsafe p` | Raw pointer      | no checking — only inside `unsafe {}`              |

### Examples

```c
int x = 42;

// Immutable borrow — does not transfer ownership, read only
int& read_x = &x;
printf("%d\n", *read_x);   // OK

// Mutable borrow — can write, but blocks other borrows
int&mut edit_x = &mut x;
*edit_x = 100;              // OK

// Owning pointer (heap)
int* p = new int(99);       // p owns the memory
// ...when leaving scope, free(p) is inserted automatically

// Raw pointer — only inside unsafe
unsafe {
    int* unsafe raw = &x;   // no borrow checker protection
}
```

---

## 5. Ownership

### Ownership rules

1. **Every value has exactly one owner** at all times
2. **When the owner goes out of scope, the value is freed** (automatic free for heap)
3. **Ownership is transferred with `move()`** — the original becomes invalid

### Move semantics

```c
int* a = new int(100);    // a is the owner
int* b = move(a);         // b takes ownership — a is now invalid

printf("%d\n", *b);       // OK — b is the owner
printf("%d\n", *a);       // ERROR[E0382]: use of moved value 'a'
                          // The compiler rejects this at compile time
```

### Why this matters (vs C)

```c
// In C — this compiles and crashes at runtime:
int* p = malloc(sizeof(int));
*p = 42;
int* q = p;     // q and p point to the same location
free(p);        // free the memory
free(q);        // DOUBLE FREE — crash or memory corruption

// In Ferrum — the compiler rejects it:
int* p = new int(42);
int* q = move(p);   // p is now invalid
// free(p) no longer happens — p was moved
// free(q) happens automatically when leaving scope
```

### Scope and automatic free

```c
int main() {
    // outer scope
    int* a = new int(1);

    {
        // inner scope
        int* b = new int(2);
        printf("%d\n", *b);
        // <- free(b) inserted here automatically
    }

    printf("%d\n", *a);
    // <- free(a) inserted here automatically
    return 0;
}
```

---

## 6. Borrow Checker

The borrow checker is Ferrum's safety engine. It verifies at compile time that there are no memory violations.

### Rules

| Rule | Description |
|------|-------------|
| **One reader or one writer** | Multiple `&` at the same time OK. One `&mut` blocks all others. |
| **No use after move** | After `move(x)`, `x` is invalid until reassigned. |
| **No outliving borrow** | A borrow cannot live longer than the value it borrows from. |
| **No mutation while borrowed** | If `&x` exists, `x` cannot be modified. |

### Examples of errors caught

```c
// ERROR: two simultaneous mutable borrows
int x = 10;
int&mut a = &mut x;
int&mut b = &mut x;    // error[E0502]: cannot borrow 'x' as mutable more than once

// ERROR: mutable borrow with active immutable borrow
int x = 10;
int& read = &x;
int&mut edit = &mut x;  // error[E0502]: cannot borrow 'x' as mutable
                         //              because it is also borrowed as immutable

// ERROR: mutation while borrowed
int x = 10;
int& ref_x = &x;
x = 20;               // error[E0596]: cannot assign to 'x' because it is borrowed

// OK: multiple immutable borrows
int x = 10;
int& a = &x;
int& b = &x;
int& c = &x;          // perfectly valid — read only
```

---

## 7. Lifetimes

Lifetimes allow explicitly annotating how long a borrow lives. Ferrum infers them automatically in most cases.

### Syntax

```c
// Explicit lifetime annotation
int& 'a ref = &value;    // ref has lifetime 'a

// In function parameters
int& 'a return_larger(int& 'a x, int& 'a y) {
    if (*x > *y) return x;
    return y;
}
```

### Implicit lifetime (the common case)

```c
// Ferrum infers lifetimes automatically
int x = 42;
int& ref_x = &x;     // inferred lifetime: ref_x lives as long as x
printf("%d\n", *ref_x);  // OK
// ref_x goes out of scope before x — all good
```

### Dangling borrow error

```c
// Ferrum catches this:
int& get_ref() {
    int local = 42;
    int& ref = &local;
    return ref;        // error[E0597]: 'ref' borrows 'local' which is dropped here
}
// local is destroyed when the function returns — ref would be dangling
```

---

## 8. Unsafe

The `unsafe` block allows operations that the borrow checker cannot verify statically. It is the escape hatch for low-level code.

### What requires unsafe

- Declaring raw pointers (`int* unsafe p`)
- Dereferencing raw pointers
- Interoperating with C code that uses unannotated pointers

### Syntax

```c
int x = 42;

// OUTSIDE unsafe — the compiler protects:
int* unsafe bad = &x;   // error[E0133]: unsafe pointer can only be declared
                         //              inside an unsafe block

// INSIDE unsafe — explicitly allowed:
unsafe {
    int* unsafe raw = &x;
    *raw = 100;             // OK inside unsafe
    printf("%d\n", *raw);   // OK
}
```

### Why unsafe exists

95% of code can be safe. The 5% that needs low-level operations (drivers, FFI, custom data structures) uses `unsafe` in an **explicit and isolated** way, which makes security audits easier.

```c
// The unsafe is isolated and easy to audit:
unsafe {
    // Only this block needs manual review
    int* unsafe mem = custom_mmap();
    *mem = initialize();
}
// The rest of the code is verified automatically
```

---

## 9. Generics

Ferrum supports generic functions and structs with syntax similar to C++.

### Generic functions

```c
// Generic function — T is the type parameter
T maximum<T>(T a, T b) {
    if (a > b) return a;
    return b;
}

int main() {
    int   res_int   = maximum<int>(3, 7);       // T = int
    float res_float = maximum<float>(1.5, 2.3); // T = float
}
```

### Generic structs

```c
// Generic pair — A and B are arbitrary types
struct Pair<A, B> {
    A first;
    B second;
}

// Usage:
Pair<int, float> point;
```

### Current status

Generics are **checked at compile time** (type checker). Monomorphization (generating LLVM code per instantiation) is work in progress — version 0.3.

---

## 10. Structs

### Declaration

```c
struct Point {
    int x;
    int y;
}

struct Rectangle {
    Point origin;
    int width;
    int height;
}
```

### Structs and ownership

```c
struct Buffer {
    int* data;    // the struct owns this memory
    int  size;
}

// When leaving scope, data is freed automatically
Buffer buf;
buf.data = new int(1024);
buf.size = 1024;
// <- free(buf.data) inserted here automatically
```

---

## 11. Functions

### Basic syntax

```c
// Simple function
int add(int a, int b) {
    return a + b;
}

// No return value
void print(int x) {
    printf("value: %d\n", x);
}

// Returning a pointer
int* create_int(int value) {
    int* p = new int(value);
    return move(p);    // transfers ownership to the caller
}
```

### Unsafe functions

```c
// Function that works with raw pointers
unsafe int* read_memory_direct(int* unsafe addr) {
    return addr;
}
```

### Extern (interop with C/C++)

```c
extern "C" {
    // Declare external C functions
    int c_function(int x, int y);
}

extern "C++" {
    // Declare external C++ functions
    void cpp_method(void* obj);
}
```

---

## 12. Control Flow

### if / else

```c
if (x > 0) {
    printf("positive\n");
} else if (x < 0) {
    printf("negative\n");
} else {
    printf("zero\n");
}
```

### while

```c
int i = 0;
while (i < 10) {
    printf("%d\n", i);
    i = i + 1;
}
```

### for

Ferrum accepts both forms — with declaration (Ferrum style) or with expression (C style):

```c
// Ferrum style — declares variable in the for
for (int i = 0; i < 10; i = i + 1) {
    printf("%d\n", i);
}

// C style — variable declared before
int i;
for (i = 0; i < 10; i = i + 1) {
    printf("%d\n", i);
}
```

---

## 13. Imports and C/C++ Interoperability

### Import forms

```c
// C style (preprocessor) — accepted by Ferrum
#include <stdio.h>
#include <stdlib.h>
#include "my_header.h"

// Ferrum style — exact equivalent
import <stdio.h>;
import <stdlib.h>;
import "my_header.feh";
```

Both forms are **100% equivalent**. Ferrum converts `#include` to `import` internally during the lexing phase.

### Automatically recognized C headers

When one of these headers is imported, Ferrum pre-registers the functions in the type checker:

| Header | Registered functions |
|--------|----------------------|
| `<stdio.h>` | `printf`, `scanf`, `puts`, `fprintf`, `fopen`, `fclose` |
| `<stdlib.h>` | `malloc`, `free`, `exit`, `atoi`, `rand` |
| `<string.h>` | `strlen`, `strcpy`, `strcmp`, `memcpy`, `memset` |
| `<math.h>` | `sqrt`, `pow`, `sin`, `cos`, `fabs`, `floor`, `ceil` |

### Extern blocks

```c
// Declare existing C functions
extern "C" {
    int my_c_function(int x);
    void other_function(char* str);
}

// Declare existing C++ functions
extern "C++" {
    void class_method(void* obj, int param);
}
```

---

## 14. Compiler Pipeline

```
file.fe (or file with #include / import)
    |
    v
+-----------------------------------------+
|  LEXER                                  |
|  * Tokenizes the source code            |
|  * Converts #include -> import          |
|  * Recognizes lifetimes ('a)            |
|  * Recognizes :: and Ferrum tokens      |
+------------------+----------------------+
                   | tokens
                   v
+-----------------------------------------+
|  PARSER                                 |
|  * Builds the AST                       |
|  * Parses generics <T, U>              |
|  * Parses lifetimes in types            |
|  * Parses unsafe blocks                 |
+------------------+----------------------+
                   | AST
                   v
+-----------------------------------------+
|  TYPE CHECKER (Sema)                    |
|  * Infers expression types              |
|  * Verifies function signatures         |
|  * Resolves generic parameters          |
|  * Detects type errors                  |
+------------------+----------------------+
                   | typed AST
                   v
+-----------------------------------------+
|  BORROW CHECKER                         |
|  * Verifies ownership and moves         |
|  * Verifies borrow rules                |
|  * Detects dangling borrows             |
|  * Isolates unsafe                      |
|  * Tracks lifetimes per scope           |
+------------------+----------------------+
                   | verified AST
                   v
+-----------------------------------------+
|  CODEGEN (LLVM IR)                      |
|  * Generates LLVM IR                    |
|  * Inserts free() at scope end          |
|  * Handles move as transfer             |
|  * Borrows = pointers in IR             |
+------------------+----------------------+
                   | LLVM IR (.ll)
                   v
+-----------------------------------------+
|  BACKEND (llc + gcc)                    |
|  * llc: IR -> object (.o)              |
|  * gcc: object -> binary               |
+------------------+----------------------+
                   |
                   v
            executable binary
```

### Compiler usage

```bash
# Compile to binary
./build/ferrumc file.fe -o my_program

# View the generated LLVM IR only
./build/ferrumc file.fe --emit-ir

# Run
./my_program
```

---

## 15. Compiler Errors

Ferrum uses error codes with the format `E####`:

| Code | Name | Description |
|------|------|-------------|
| `E0382` | UseAfterMove | Use of variable after `move()` |
| `E0502` | MutableBorrowConflict | Mutable borrow when immutable borrow exists (or vice versa) |
| `E0505` | UseAfterFree | Use of pointer after it has been freed |
| `E0596` | MutateWhileBorrowed | Modifying a variable that has active borrows |
| `E0597` | BorrowOutlivesOwner | Borrow lives longer than the value it borrows from |
| `E0133` | UnsafeOutsideUnsafeBlock | `unsafe` pointer outside an `unsafe {}` block |

### Example error message

```
error[E0382] line 8: use of moved value 'limit'
1 borrow error(s).

Compilation failed.
```

---

## 16. C vs Ferrum Comparison

### The same program — two perspectives

```c
// ======================================================
// C VERSION — works, but no protection
// ======================================================
#include <stdio.h>
#include <stdlib.h>

int main() {
    int* p = malloc(sizeof(int));
    *p = 42;

    int* q = p;          // q and p point to the same location
    free(p);             // p freed
    printf("%d\n", *q);  // USE-AFTER-FREE — undefined behavior
    free(q);             // DOUBLE FREE — crash

    return 0;
}
// C compiles this without any warning. Crashes at runtime.


// ======================================================
// FERRUM VERSION — same code, with protection
// ======================================================
#include <stdio.h>

int main() {
    int* p = new int(42);

    int* q = move(p);    // q takes ownership — p is now invalid
    printf("%d\n", *q);  // OK — q is the owner

    // printf("%d\n", *p); // error[E0382]: use of moved value 'p'
    //                     // Ferrum REJECTS this at compile time

    // free(q) inserted automatically here
    return 0;
}
// Ferrum catches the bug at compile time. Zero runtime crashes from this.
```

### Summary of differences

| Feature | C | Ferrum |
|---------|---|--------|
| Syntax | `int x = 0;` | `int x = 0;` <- **identical** |
| `#include` | `#include <stdio.h>` | `#include <stdio.h>` <- **identical** |
| Manual memory | `malloc` / `free` | `new` / automatic free |
| Safe pointers | No | `int& p`, `int&mut p` |
| Use-after-free | Runtime crash | Compile-time error |
| Double free | Runtime crash | Compile-time error |
| Data races | Runtime (undefined) | Compile-time error |
| Raw pointers | Default | Only in `unsafe {}` |
| Generics | No (macros/void* only) | `T func<T>(T x)` |
| Lifetimes | No | Inferred automatically |

---

## File Extensions

| Extension | Use |
|-----------|-----|
| `.fe` | Ferrum source code |
| `.feh` | Ferrum headers (equivalent to C's `.h`) |

---

*Ferrum Compiler v0.2 — built with LLVM 18*
