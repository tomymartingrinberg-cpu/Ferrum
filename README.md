# Ferrum

(I'm 11 years old and I built this with Claude Code. For any issues, open one on GitHub or email me at tomy.martin.grinberg@gmail.com. This is non-profit.)

This is licensed under the GNU GPL v3 https://github.com/tomymartingrinberg-cpu/ferrum/blob/main/LICENSE

Esto esta bajo la GNU GPL v3 https://github.com/tomymartingrinberg-cpu/ferrum/blob/main/LICENSE

**Syntax: C | Safety: compile-time checked | Ecosystem: C++**

Ferrum is a systems programming language that takes C's syntax and adds compile-time memory safety checks. If the program compiles, there are no use-after-free, double free, or dangling pointer bugs.

```c
#include <stdio.h>

int main() {
    int* p = new int(42);      // heap allocation
    int* q = move(p);          // transfers ownership — p is now invalid

    printf("q = %d\n", *q);   // OK
    // printf("%d\n", *p);     // error[E0382]: use of moved value 'p'
                               // the compiler rejects this
    return 0;
    // free(q) inserted automatically
}
```

> Also available in: [Español](README.es.md)

---

## Why Ferrum

C is fast and simple, but it doesn't protect the programmer from memory errors. Ferrum fixes that without changing the syntax or adding a garbage collector.

| Bug | C | Ferrum |
|-----|---|--------|
| Use-after-free | Runtime crash | Compile-time error |
| Double free | Runtime crash | Compile-time error |
| Use-after-move | Undefined behavior | Compile-time error |
| Dangling borrow | Invalid memory access | Compile-time error |
| Uncontrolled raw pointer | No protection | Only inside `unsafe {}` |

---

## Features

- **C compatible** — all valid C code is valid Ferrum code
- **`#include` and `import`** — both forms are accepted
- **Ownership and move semantics** — `new`, `move()`, automatic free when leaving scope
- **Borrow checker** — `int& p` (immutable), `int&mut p` (mutable), enforced at compile time
- **Lifetimes** — `'a` annotations, automatically inferred in most cases
- **Explicit unsafe** — `unsafe {}` isolates low-level code
- **Generics** — `T func<T>(T x)`, `struct Pair<A, B>`
- **Compiles to native binary** — via LLVM 18

---

## Installation

### Requirements

- `g++` with C++20
- LLVM 18 (`llvm-config`, `llc`)
- `gcc` (for linking)

```bash
# Ubuntu / Debian
sudo apt install llvm-18 llvm-18-dev gcc g++ build-essential
```

### Build the compiler

```bash
git clone https://github.com/tomymartingrinberg-cpu/ferrum
cd ferrum
bash build.sh
```

This produces `build/ferrumc`.

---

## Usage

```bash
# Compile a .fe file to a binary
./build/ferrumc file.fe -o my_program

# View the generated LLVM IR
./build/ferrumc file.fe --emit-ir

# Run
./my_program
```

---

## Examples

### Hello world (C style)

```c
#include <stdio.h>

int main() {
    printf("Hello from Ferrum!\n");
    return 0;
}
```

### Ownership and move

```c
#include <stdio.h>

int main() {
    int* a = new int(100);   // a is the owner
    int* b = move(a);        // b takes ownership

    printf("%d\n", *b);      // OK
    // *a                    // error[E0382]: use of moved value 'a'

    return 0;
    // free(b) automatic
}
```

### Borrows

```c
#include <stdio.h>

int main() {
    int x = 42;

    int& read_x  = &x;       // immutable borrow — read only
    int&mut edit = &mut x;   // mutable borrow — read and write

    // int& other = &x;      // error[E0502]: x already has a mutable borrow

    *edit = 99;
    printf("%d\n", x);       // 99
    return 0;
}
```

### Unsafe

```c
#include <stdio.h>

int main() {
    int x = 10;

    // int* unsafe p = &x;   // error[E0133]: outside unsafe block

    unsafe {
        int* unsafe p = &x;  // OK inside unsafe
        printf("%d\n", *p);
    }
    return 0;
}
```

### Generics

```c
#include <stdio.h>

int maximum<T>(T a, T b) {
    if (a > b) return a;
    return b;
}

int main() {
    printf("%d\n", maximum<int>(3, 7));    // 7
    return 0;
}
```

---

## Compiler errors

| Code | Description |
|------|-------------|
| `E0382` | Use of variable after `move()` |
| `E0502` | Borrow conflict (mutable + immutable) |
| `E0505` | Use of pointer after free |
| `E0596` | Mutation while borrow is active |
| `E0597` | Borrow outlives the value it borrows from |
| `E0133` | `unsafe` pointer outside `unsafe {}` block |

---

## Project structure

```
ferrum/
├── include/ferrum/
│   ├── Token.h          # Language tokens
│   ├── Lexer.h          # Lexer
│   ├── AST.h            # Abstract syntax tree
│   ├── Parser.h         # Parser
│   ├── TypeChecker.h    # Type system (Sema)
│   ├── BorrowChecker.h  # Ownership verifier
│   └── Codegen.h        # LLVM IR generator
├── src/
│   ├── lexer/
│   ├── parser/
│   ├── sema/
│   ├── borrow/
│   ├── codegen/
│   └── driver/main.cpp  # Compiler entry point
├── tests/
│   ├── hello.fe
│   ├── counter.fe
│   ├── factorial_ferrum.fe
│   ├── test_lexer.cpp
│   └── test_borrow.cpp
├── docs/
│   └── SPEC.md          # Full language specification
└── build.sh             # Build script
```

---

## Compiler pipeline

```
.fe / .c source
      │
      ▼
   Lexer          (#include → import, lifetimes 'a, tokens)
      │
      ▼
   Parser         (AST, generics, unsafe blocks)
      │
      ▼
   TypeChecker    (type inference, generics, C headers)
      │
      ▼
   BorrowChecker  (ownership, moves, borrows, lifetimes, unsafe)
      │
      ▼
   Codegen        (LLVM IR, automatic free() per scope)
      │
      ▼
   llc + gcc      (native binary)
```

---

## Full documentation

See [`docs/SPEC.md`](docs/SPEC.md) for the full language specification.

---

*Ferrum-language Compiler v0.3 — LLVM 18*
