# Ferrum

(Tengo 11 años y lo hice con Claude Code, cualquier fallo, ponganlo en el GitHub o por mail al tomy.martin.grinberg@gmail.com. Esto esta hecho sin fines de lucro)

**Syntax: C | Safety: compile-time checked | Ecosystem: C++**

Ferrum es un lenguaje de programación de sistemas que toma la sintaxis de C y le agrega verificaciones de seguridad de memoria en tiempo de compilación. Si el programa compila, no hay use-after-free, double free, ni dangling pointers.

```c
#include <stdio.h>

int main() {
    int* p = new int(42);      // heap allocation
    int* q = move(p);          // transfiere ownership — p queda inválido

    printf("q = %d\n", *q);   // OK
    // printf("%d\n", *p);     // error[E0382]: use of moved value 'p'
                               // el compilador rechaza esto
    return 0;
    // free(q) insertado automáticamente
}
```

---

## Por qué Ferrum

C es rápido y simple, pero no protege al programador de errores de memoria. Ferrum resuelve eso sin cambiar la sintaxis ni agregar un garbage collector.

| Bug | C | Ferrum |
|-----|---|--------|
| Use-after-free | Crash en runtime | Error de compilación |
| Double free | Crash en runtime | Error de compilación |
| Use-after-move | Undefined behavior | Error de compilación |
| Dangling borrow | Acceso inválido | Error de compilación |
| Raw pointer sin control | Sin protección | Solo dentro de `unsafe {}` |

---

## Características

- **C compatible** — todo código C válido es código Ferrum válido
- **`#include` e `import`** — ambas formas son aceptadas
- **Ownership y move semantics** — `new`, `move()`, free automático al salir del scope
- **Borrow checker** — `int& p` (inmutable), `int&mut p` (mutable), reglas en compilación
- **Lifetimes** — anotaciones `'a` inferidas automáticamente en la mayoría de los casos
- **Unsafe explícito** — `unsafe {}` aísla el código de bajo nivel
- **Genéricos** — `T funcion<T>(T x)`, `struct Par<A, B>`
- **Compila a binario nativo** — via LLVM 18

---

## Instalación

### Requisitos

- `g++` con C++20
- LLVM 18 (`llvm-config`, `llc`)
- `gcc` (para linkear)

```bash
# Ubuntu / Debian
sudo apt install llvm-18 llvm-18-dev gcc g++ build-essential
```

### Compilar el compilador

```bash
git clone https://github.com/tomymartingrinberg-cpu/ferrum
cd ferrum
bash build.sh
```

Esto produce `build/ferrumc`.

---

## Uso

```bash
# Compilar un archivo .fe a binario
./build/ferrumc archivo.fe -o mi_programa

# Ver el LLVM IR generado
./build/ferrumc archivo.fe --emit-ir

# Ejecutar
./mi_programa
```

---

## Ejemplos

### Hola mundo (estilo C)

```c
#include <stdio.h>

int main() {
    printf("Hola desde Ferrum!\n");
    return 0;
}
```

### Ownership y move

```c
#include <stdio.h>

int main() {
    int* a = new int(100);   // a es dueño
    int* b = move(a);        // b toma el ownership

    printf("%d\n", *b);      // OK
    // *a                    // error[E0382]: use of moved value 'a'

    return 0;
    // free(b) automático
}
```

### Borrows

```c
#include <stdio.h>

int main() {
    int x = 42;

    int& leer = &x;          // borrow inmutable — solo lectura
    int&mut editar = &mut x; // borrow mutable — lectura y escritura

    // int& otro = &x;       // error[E0502]: x ya tiene borrow mutable

    *editar = 99;
    printf("%d\n", x);       // 99
    return 0;
}
```

### Unsafe

```c
#include <stdio.h>

int main() {
    int x = 10;

    // int* unsafe p = &x;   // error[E0133]: fuera de unsafe block

    unsafe {
        int* unsafe p = &x;  // OK dentro de unsafe
        printf("%d\n", *p);
    }
    return 0;
}
```

### Genéricos

```c
#include <stdio.h>

int maximo<T>(T a, T b) {
    if (a > b) return a;
    return b;
}

int main() {
    printf("%d\n", maximo<int>(3, 7));     // 7
    return 0;
}
```

---

## Errores del compilador

| Código | Descripción |
|--------|-------------|
| `E0382` | Uso de variable después de `move()` |
| `E0502` | Conflicto de borrows (mutable + inmutable) |
| `E0505` | Uso de puntero después de liberar |
| `E0596` | Mutación con borrow activo |
| `E0597` | Borrow que vive más que el valor que presta |
| `E0133` | Puntero `unsafe` fuera de bloque `unsafe {}` |

---

## Estructura del proyecto

```
ferrum/
├── include/ferrum/
│   ├── Token.h          # Tokens del lenguaje
│   ├── Lexer.h          # Analizador léxico
│   ├── AST.h            # Árbol de sintaxis abstracta
│   ├── Parser.h         # Parser
│   ├── TypeChecker.h    # Sistema de tipos (Sema)
│   ├── BorrowChecker.h  # Verificador de ownership
│   └── Codegen.h        # Generador de LLVM IR
├── src/
│   ├── lexer/
│   ├── parser/
│   ├── sema/
│   ├── borrow/
│   ├── codegen/
│   └── driver/main.cpp  # Punto de entrada del compilador
├── tests/
│   ├── hello.fe
│   ├── counter.fe
│   ├── factorial_ferrum.fe
│   ├── test_lexer.cpp
│   └── test_borrow.cpp
├── docs/
│   └── SPEC.md          # Especificación completa del lenguaje
└── build.sh             # Script de compilación
```

---

## Pipeline del compilador

```
.fe / .c source
      │
      ▼
   Lexer          (#include → import, lifetimes 'a, tokens)
      │
      ▼
   Parser         (AST, genéricos, unsafe blocks)
      │
      ▼
   TypeChecker    (inferencia de tipos, genéricos, C headers)
      │
      ▼
   BorrowChecker  (ownership, moves, borrows, lifetimes, unsafe)
      │
      ▼
   Codegen        (LLVM IR, free() automático por scope)
      │
      ▼
   llc + gcc      (binario nativo)
```

---

## Documentación completa

Ver [`docs/SPEC.md`](docs/SPEC.md) para la especificación completa del lenguaje.

---

*Ferrum Compiler v0.2 — LLVM 18*
