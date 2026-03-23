# Especificación del Lenguaje Ferrum v0.3

> **Syntax: C | Safety: compile-time checked | Ecosystem: C++**

Ferrum es un lenguaje de programación de sistemas que combina la sintaxis familiar de C con verificaciones de seguridad de memoria en tiempo de compilación, manteniendo interoperabilidad total con el ecosistema de C y C++.

> También disponible en: [English](SPEC.md)

---

## Tabla de Contenidos

1. [Filosofía de Diseño](#1-filosofía-de-diseño)
2. [Relación con C](#2-relación-con-c)
3. [Variables y Tipos](#3-variables-y-tipos)
4. [Punteros y Referencias](#4-punteros-y-referencias)
5. [Ownership (Propiedad)](#5-ownership-propiedad)
6. [Borrow Checker](#6-borrow-checker)
7. [Lifetimes (Tiempos de Vida)](#7-lifetimes-tiempos-de-vida)
8. [Unsafe](#8-unsafe)
9. [Genéricos](#9-genéricos)
10. [Structs](#10-structs)
11. [Funciones](#11-funciones)
12. [Control de Flujo](#12-control-de-flujo)
13. [Importaciones e Interoperabilidad con C/C++](#13-importaciones-e-interoperabilidad-con-cc)
14. [Pipeline del Compilador](#14-pipeline-del-compilador)
15. [Errores del Compilador](#15-errores-del-compilador)
16. [Comparación C vs Ferrum](#16-comparación-c-vs-ferrum)

---

## 1. Filosofía de Diseño

Ferrum parte de una pregunta simple: **¿qué pasaría si C tuviera un borrow checker integrado?**

### Principios fundamentales

| # | Principio |
|---|-----------|
| 1 | **Si compila, es memory-safe** — no hay undefined behavior por violaciones de ownership |
| 2 | **C válido es Ferrum válido** — código C existente funciona sin modificaciones |
| 3 | **Cero overhead en runtime** — todas las verificaciones son en tiempo de compilación |
| 4 | **Sin garbage collector** — el programador controla la memoria; el compilador la libera |
| 5 | **C++ es ciudadano de primera** — headers, STL y ABIs de C++ funcionan directo |

### Lo que Ferrum agrega a C

```
C puro                          Ferrum
──────────────────────────────────────────────────────────────
int* p = malloc(sizeof(int));   int* p = new int(42);
*p = 42;                        // free() insertado automaticamente
free(p);                        // al salir del scope

// Sin proteccion:              // Protegido en compilacion:
int* q = p;                     int* q = move(p);
free(p);  // double free!       free(p);  // ERROR: p was moved
*p = 1;   // use-after-free!    *p = 1;   // ERROR: use of moved value
```

---

## 2. Relación con C

### C es un subconjunto de Ferrum

Todo programa C válido es también un programa Ferrum válido. La única diferencia de sintaxis es que C usa `#include` y Ferrum usa `import`, pero **ambas formas son aceptadas**:

```c
// Estilo C — funciona en Ferrum
#include <stdio.h>
#include <stdlib.h>

// Estilo Ferrum — equivalente exacto
import <stdio.h>;
import <stdlib.h>;
```

### Qué hace Ferrum con código C

Ferrum no rechaza el código C — lo **analiza y agrega seguridad encima**:

```c
// Esto es C puro — Ferrum lo acepta
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

Ferrum ejecuta el **borrow checker y el type checker** encima de este código. Si hay violaciones de memoria, las detecta. Si no las hay, compila normal.

### Lo que Ferrum detecta que C no detecta

| Bug | C | Ferrum |
|-----|---|--------|
| Use-after-free | Crash en runtime | `error[E0505]` en compilación |
| Double free | Crash en runtime | `error[E0382]` en compilación |
| Use-after-move | Comportamiento undefined | `error[E0382]` en compilación |
| Dangling borrow | Acceso a memoria inválida | `error[E0597]` en compilación |
| Mutación con borrow activo | Data race en runtime | `error[E0596]` en compilación |
| Raw pointer fuera de unsafe | Sin protección | `error[E0133]` en compilación |

---

## 3. Variables y Tipos

### Tipos primitivos

| Tipo Ferrum | Equivalente C | Tamaño | Descripción |
|-------------|---------------|--------|-------------|
| `int`       | `int`         | 32-bit | Entero con signo |
| `float`     | `double`      | 64-bit | Punto flotante |
| `char`      | `char`        | 8-bit  | Carácter / byte |
| `bool`      | `_Bool`       | 1-bit  | Booleano |
| `void`      | `void`        | —      | Sin valor |

### Declaración de variables

```c
// Con tipo explícito
int x = 42;
float pi = 3.14;
bool activo = true;
char letra = 'A';

// Sin inicializar (valor indefinido — igual que C)
int contador;
```

### Variables en el stack vs heap

```c
int x = 10;          // stack — se libera al salir del scope automáticamente
int* p = new int(10); // heap  — Ferrum inserta free() al salir del scope
```

---

## 4. Punteros y Referencias

Ferrum tiene cuatro tipos de punteros, cada uno con semántica distinta:

### Tabla de punteros

| Sintaxis        | Nombre           | Semántica                                        |
|-----------------|------------------|--------------------------------------------------|
| `int* p`        | Owning pointer   | p es el dueño — se libera cuando p sale de scope |
| `int& p`        | Borrow inmutable | lectura — múltiples borrows simultáneos OK       |
| `int&mut p`     | Borrow mutable   | lectura+escritura — solo uno a la vez            |
| `int* unsafe p` | Raw pointer      | sin verificación — solo dentro de `unsafe {}`   |

### Ejemplos

```c
int x = 42;

// Borrow inmutable — no transfiere ownership, solo lee
int& leer_x = &x;
printf("%d\n", *leer_x);   // OK

// Borrow mutable — puede escribir, pero bloquea otros borrows
int&mut editar_x = &mut x;
*editar_x = 100;            // OK

// Owning pointer (heap)
int* p = new int(99);       // p es dueño de la memoria
// ...al salir del scope, free(p) se inserta automáticamente

// Raw pointer — solo en unsafe
unsafe {
    int* unsafe raw = &x;   // sin protección del borrow checker
}
```

---

## 5. Ownership (Propiedad)

### Reglas de ownership

1. **Cada valor tiene exactamente un dueño** en todo momento
2. **Cuando el dueño sale de scope, el valor se libera** (free automático para heap)
3. **El ownership se transfiere con `move()`** — el original queda inválido

### Move semantics

```c
int* a = new int(100);    // a es el dueño
int* b = move(a);         // b toma el ownership — a queda inválido

printf("%d\n", *b);       // OK — b es el dueño
printf("%d\n", *a);       // ERROR[E0382]: use of moved value 'a'
                          // El compilador rechaza esto en compilación
```

### Por qué esto importa (vs C)

```c
// En C — esto compila y explota en runtime:
int* p = malloc(sizeof(int));
*p = 42;
int* q = p;     // q y p apuntan al mismo lugar
free(p);        // liberamos la memoria
free(q);        // DOUBLE FREE — crash o corrupción de memoria

// En Ferrum — el compilador lo rechaza:
int* p = new int(42);
int* q = move(p);   // p queda inválido
// free(p) ya no ocurre — p fue movido
// free(q) ocurre automáticamente al salir del scope
```

### Scope y free automático

```c
int main() {
    // scope externo
    int* a = new int(1);

    {
        // scope interno
        int* b = new int(2);
        printf("%d\n", *b);
        // <- free(b) insertado aquí automáticamente
    }

    printf("%d\n", *a);
    // <- free(a) insertado aquí automáticamente
    return 0;
}
```

---

## 6. Borrow Checker

El borrow checker es el motor de seguridad de Ferrum. Verifica en tiempo de compilación que no haya violaciones de memoria.

### Reglas

| Regla | Descripción |
|-------|-------------|
| **Una lectura o una escritura** | Múltiples `&` simultáneos OK. Un `&mut` bloquea todos los demás. |
| **No usar después de mover** | Tras `move(x)`, `x` es inválido hasta reasignación. |
| **No prestar lo que no existe** | Un borrow no puede vivir más que el valor que presta. |
| **No mutar mientras hay borrows** | Si existe un `&x`, no se puede modificar `x`. |

### Ejemplos de errores que detecta

```c
// ERROR: dos borrows mutables simultáneos
int x = 10;
int&mut a = &mut x;
int&mut b = &mut x;    // error[E0502]: cannot borrow 'x' as mutable more than once

// ERROR: borrow mutable con borrow inmutable activo
int x = 10;
int& leer = &x;
int&mut editar = &mut x;  // error[E0502]: cannot borrow 'x' as mutable
                           //              because it is also borrowed as immutable

// ERROR: mutación mientras hay borrow
int x = 10;
int& ref_x = &x;
x = 20;               // error[E0596]: cannot assign to 'x' because it is borrowed

// OK: múltiples borrows inmutables
int x = 10;
int& a = &x;
int& b = &x;
int& c = &x;          // perfectamente válido — solo lectura
```

---

## 7. Lifetimes (Tiempos de Vida)

Los lifetimes permiten anotar explícitamente cuánto tiempo vive un borrow. Ferrum los infiere automáticamente en la mayoría de los casos.

### Sintaxis

```c
// Anotación explícita de lifetime
int& 'a ref = &valor;    // ref tiene lifetime 'a

// En parámetros de función
int& 'a devolver_mayor(int& 'a x, int& 'a y) {
    if (*x > *y) return x;
    return y;
}
```

### Lifetime implícito (el caso común)

```c
// Ferrum infiere los lifetimes automáticamente
int x = 42;
int& ref_x = &x;     // lifetime inferido: ref_x vive tanto como x
printf("%d\n", *ref_x);  // OK
// ref_x sale de scope antes que x — todo bien
```

### Error de dangling borrow

```c
// Ferrum detecta esto:
int& obtener_ref() {
    int local = 42;
    int& ref = &local;
    return ref;        // error[E0597]: 'ref' borrows 'local' which is dropped here
}
// local se destruye al salir de la función — ref quedaría colgando
```

---

## 8. Unsafe

El bloque `unsafe` permite operaciones que el borrow checker no puede verificar estáticamente. Es el escape hatch para código de bajo nivel.

### Qué requiere unsafe

- Declarar punteros raw (`int* unsafe p`)
- Desreferenciar punteros raw
- Interoperar con código C que usa punteros sin anotar

### Sintaxis

```c
int x = 42;

// FUERA de unsafe — el compilador protege:
int* unsafe bad = &x;   // error[E0133]: unsafe pointer can only be declared
                         //              inside an unsafe block

// DENTRO de unsafe — permitido explícitamente:
unsafe {
    int* unsafe raw = &x;
    *raw = 100;             // OK dentro de unsafe
    printf("%d\n", *raw);   // OK
}
```

### Por qué existe unsafe

El 95% del código puede ser safe. El 5% que necesita operaciones de bajo nivel (drivers, FFI, estructuras de datos custom) usa `unsafe` de forma **explícita y aislada**, lo que facilita auditorías de seguridad.

```c
// El unsafe está aislado y es fácil de auditar:
unsafe {
    // Solo este bloque necesita revisión manual
    int* unsafe mem = mmap_personalizado();
    *mem = inicializar();
}
// El resto del código es verificado automáticamente
```

---

## 9. Genéricos

Ferrum soporta funciones y structs genéricos con sintaxis similar a C++.

### Funciones genéricas

```c
// Función genérica — T es el tipo parametrizado
T maximo<T>(T a, T b) {
    if (a > b) return a;
    return b;
}

int main() {
    int   res_int   = maximo<int>(3, 7);       // T = int
    float res_float = maximo<float>(1.5, 2.3); // T = float
}
```

### Structs genéricos

```c
// Par genérico — A y B son tipos arbitrarios
struct Par<A, B> {
    A primero;
    B segundo;
}

// Uso:
Par<int, float> punto;
```

### Estado actual

Los genéricos son **verificados en tiempo de compilación** (type checker). La monomorphización (generación de código LLVM por cada instanciación) es trabajo en progreso.

---

## 10. Structs

### Declaración

```c
struct Punto {
    int x;
    int y;
}

struct Rectangulo {
    Punto origen;
    int ancho;
    int alto;
}
```

### Structs y ownership

```c
struct Buffer {
    int* datos;    // el struct es dueño de esta memoria
    int  tamanio;
}

// Al salir del scope, datos se libera automáticamente
Buffer buf;
buf.datos = new int(1024);
buf.tamanio = 1024;
// <- free(buf.datos) insertado aquí automáticamente
```

---

## 11. Funciones

### Sintaxis básica

```c
// Función simple
int sumar(int a, int b) {
    return a + b;
}

// Sin retorno
void imprimir(int x) {
    printf("valor: %d\n", x);
}

// Con puntero como retorno
int* crear_entero(int valor) {
    int* p = new int(valor);
    return move(p);    // transfiere ownership al llamador
}
```

### Funciones unsafe

```c
// Función que trabaja con raw pointers
unsafe int* leer_memoria_directa(int* unsafe addr) {
    return addr;
}
```

### Extern (interop con C/C++)

```c
extern "C" {
    // Declara funciones C externas
    int funcion_c(int x, int y);
}

extern "C++" {
    // Declara funciones C++ externas
    void metodo_cpp(void* obj);
}
```

---

## 12. Control de Flujo

### if / else

```c
if (x > 0) {
    printf("positivo\n");
} else if (x < 0) {
    printf("negativo\n");
} else {
    printf("cero\n");
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

Ferrum acepta ambas formas — con declaración (Ferrum) o con expresión (C):

```c
// Estilo Ferrum — declara la variable en el for
for (int i = 0; i < 10; i = i + 1) {
    printf("%d\n", i);
}

// Estilo C — variable declarada antes
int i;
for (i = 0; i < 10; i = i + 1) {
    printf("%d\n", i);
}
```

---

## 13. Importaciones e Interoperabilidad con C/C++

### Formas de importar

```c
// Estilo C (preprocessor) — aceptado por Ferrum
#include <stdio.h>
#include <stdlib.h>
#include "mi_header.h"

// Estilo Ferrum — equivalente exacto
import <stdio.h>;
import <stdlib.h>;
import "mi_header.feh";
```

Ambas formas son **100% equivalentes**. Ferrum convierte `#include` a `import` internamente durante la fase de lexing.

### Headers de C reconocidos automáticamente

Cuando se importa alguno de estos headers, Ferrum pre-registra las funciones en el type checker:

| Header | Funciones registradas |
|--------|----------------------|
| `<stdio.h>` | `printf`, `scanf`, `puts`, `fprintf`, `fopen`, `fclose` |
| `<stdlib.h>` | `malloc`, `free`, `exit`, `atoi`, `rand` |
| `<string.h>` | `strlen`, `strcpy`, `strcmp`, `memcpy`, `memset` |
| `<math.h>` | `sqrt`, `pow`, `sin`, `cos`, `fabs`, `floor`, `ceil` |

### Extern blocks

```c
// Declarar funciones C existentes
extern "C" {
    int mi_funcion_c(int x);
    void otra_funcion(char* str);
}

// Declarar funciones C++ existentes
extern "C++" {
    void metodo_de_clase(void* obj, int param);
}
```

---

## 14. Pipeline del Compilador

```
archivo.fe (o archivo con #include / import)
    |
    v
+-----------------------------------------+
|  LEXER                                  |
|  * Tokeniza el código fuente            |
|  * Convierte #include -> import         |
|  * Reconoce lifetimes ('a)              |
|  * Reconoce :: y tokens de Ferrum       |
+------------------+----------------------+
                   | tokens
                   v
+-----------------------------------------+
|  PARSER                                 |
|  * Construye el AST                     |
|  * Parsea genéricos <T, U>              |
|  * Parsea lifetimes en tipos            |
|  * Parsea bloques unsafe                |
+------------------+----------------------+
                   | AST
                   v
+-----------------------------------------+
|  TYPE CHECKER (Sema)                    |
|  * Infiere tipos de expresiones         |
|  * Verifica firmas de funciones         |
|  * Resuelve parámetros genéricos        |
|  * Detecta errores de tipos             |
+------------------+----------------------+
                   | AST con tipos
                   v
+-----------------------------------------+
|  BORROW CHECKER                         |
|  * Verifica ownership y moves           |
|  * Verifica reglas de borrows           |
|  * Detecta dangling borrows             |
|  * Aísla unsafe                         |
|  * Rastrea lifetimes por scope          |
+------------------+----------------------+
                   | AST verificado
                   v
+-----------------------------------------+
|  CODEGEN (LLVM IR)                      |
|  * Genera LLVM IR                       |
|  * Inserta free() al final de scope     |
|  * Maneja move como transferencia       |
|  * Borrows = punteros en IR             |
+------------------+----------------------+
                   | LLVM IR (.ll)
                   v
+-----------------------------------------+
|  BACKEND (llc + gcc)                    |
|  * llc: IR -> objeto (.o)              |
|  * gcc: objeto -> binario              |
+------------------+----------------------+
                   |
                   v
              binario ejecutable
```

### Uso del compilador

```bash
# Compilar a binario
./build/ferrumc archivo.fe -o mi_programa

# Solo ver el LLVM IR generado
./build/ferrumc archivo.fe --emit-ir

# Ejecutar
./mi_programa
```

---

## 15. Errores del Compilador

Ferrum usa códigos de error con el formato `E####`:

| Código | Nombre | Descripción |
|--------|--------|-------------|
| `E0382` | UseAfterMove | Uso de variable después de `move()` |
| `E0502` | MutableBorrowConflict | Borrow mutable cuando ya hay borrow inmutable (o viceversa) |
| `E0505` | UseAfterFree | Uso de puntero después de liberar |
| `E0596` | MutateWhileBorrowed | Modificar variable que tiene borrows activos |
| `E0597` | BorrowOutlivesOwner | Borrow que vive más que el valor que presta |
| `E0133` | UnsafeOutsideUnsafeBlock | Puntero `unsafe` fuera de bloque `unsafe {}` |

### Ejemplo de mensaje de error

```
error[E0382] line 8: use of moved value 'limite'
1 borrow error(s).

Compilation failed.
```

---

## 16. Comparación C vs Ferrum

### El mismo programa — dos perspectivas

```c
// ======================================================
// VERSIÓN C — funciona, pero sin protección
// ======================================================
#include <stdio.h>
#include <stdlib.h>

int main() {
    int* p = malloc(sizeof(int));
    *p = 42;

    int* q = p;          // q y p apuntan al mismo lugar
    free(p);             // p liberado
    printf("%d\n", *q);  // USE-AFTER-FREE — undefined behavior
    free(q);             // DOUBLE FREE — crash

    return 0;
}
// C compila esto sin ningún warning. Explota en runtime.


// ======================================================
// VERSIÓN FERRUM — mismo código, con protección
// ======================================================
#include <stdio.h>

int main() {
    int* p = new int(42);

    int* q = move(p);    // q toma ownership — p queda inválido
    printf("%d\n", *q);  // OK — q es el dueño

    // printf("%d\n", *p); // error[E0382]: use of moved value 'p'
    //                     // Ferrum RECHAZA esto en compilación

    // free(q) insertado automáticamente aquí
    return 0;
}
// Ferrum detecta el bug en compilación. Cero runtime crashes por esto.
```

### Resumen de diferencias

| Característica | C | Ferrum |
|----------------|---|--------|
| Sintaxis | `int x = 0;` | `int x = 0;` <- **igual** |
| `#include` | `#include <stdio.h>` | `#include <stdio.h>` <- **igual** |
| Memoria manual | `malloc` / `free` | `new` / `free automático` |
| Punteros seguros | No | `int& p`, `int&mut p` |
| Use-after-free | Runtime crash | Error de compilación |
| Double free | Runtime crash | Error de compilación |
| Data races | Runtime (undefined) | Error de compilación |
| Punteros raw | Por defecto | Solo en `unsafe {}` |
| Genéricos | No (solo macros/void*) | `T funcion<T>(T x)` |
| Lifetimes | No | Inferidos automáticamente |

---

## Extensiones de Archivo

| Extensión | Uso |
|-----------|-----|
| `.fe` | Código fuente Ferrum |
| `.feh` | Headers Ferrum (equivalente a `.h` de C) |

---

*Ferrum-language Compiler v0.3 — construido con LLVM 18*
