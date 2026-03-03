# language reference

lisa is a lisp with s-expression syntax, a bytecode compiler, a stack-based vm,
and an optional jit (x86-64/arm64).

## syntax

```
; line comments
42            ; integer
3.14          ; double
"hello\n"     ; string (escapes: \n \t \r \\ \")
true false    ; booleans
nil           ; null
foo           ; symbol
(op args...)  ; call
[a b c]       ; vector (used in let bindings and fn params)
,             ; treated as whitespace
```

symbols may contain letters, digits, and `-+*/<%>=!?_&.@#`.

## special forms

### def

```
(def name value)
```

at top level, creates a global. inside a function, creates a local. the name is
registered before the value is compiled, so self-referencing closures work:

```
(def fib (fn [n]
  (if (< n 2) n
    (+ (fib (- n 1)) (fib (- n 2))))))
```

### fn

```
(fn [params...] body...)
```

anonymous function. last body expression is the return value. captures variables
from enclosing scopes (closures).

```
(def make-adder (fn [x] (fn [y] (+ x y))))
(def add5 (make-adder 5))
(println (add5 3))  ; 8
```

### let

```
(let [name1 val1 name2 val2 ...] body...)
```

local bindings. bindings are evaluated in order; later bindings can refer to
earlier ones. returns the last body expression.

```
(let [x 10 y (+ x 1)]
  (* x y))  ; 110
```

### if

```
(if cond then)
(if cond then else)
```

falsey values: `nil`, `false`. everything else is truthy. returns `nil` when
the else branch is omitted and condition is false.

### do

```
(do expr1 expr2 ... exprN)
```

evaluates expressions in order. returns the last one. creates a scope for `def`.

## operators

all operators are called as functions: `(op args...)`.

### arithmetic

| form | description |
|------|-------------|
| `(+ a b)` | add (also concatenates strings) |
| `(- a b)` | subtract |
| `(- a)` | negate |
| `(* a b)` | multiply |
| `(/ a b)` | divide (result is always double) |
| `(% a b)` | modulo |
| `(mod a b)` | modulo (alias) |

if both operands are integers, the result is an integer (except `/`). if either
is a double, the result is a double.

### comparison

| form | description |
|------|-------------|
| `(= a b)` | equal |
| `(== a b)` | equal (alias) |
| `(!= a b)` | not equal |
| `(not= a b)` | not equal (alias) |
| `(< a b)` | less than |
| `(<= a b)` | less or equal |
| `(> a b)` | greater than |
| `(>= a b)` | greater or equal |

### logical

| form | description |
|------|-------------|
| `(not v)` | logical not |

## built-in functions

### lists

| form | description |
|------|-------------|
| `(cons a b)` | construct a pair |
| `(car l)` | first element |
| `(first l)` | first element (alias) |
| `(cdr l)` | rest of list |
| `(rest l)` | rest of list (alias) |
| `(list a b ...)` | build a proper list: `(cons a (cons b ... nil))` |

### strings

| form | description |
|------|-------------|
| `(strlen s)` | string length |
| `(char-at s i)` | character at index (single-char string, or nil) |
| `(substr s start len)` | substring |
| `(str a b ...)` | concatenate values as strings |

### types

| form | description |
|------|-------------|
| `(type v)` | type name: `"nil"` `"bool"` `"int"` `"double"` `"string"` `"list"` `"fn"` `"native"` `"fiber"` `"channel"` |
| `(parse-num s)` | parse string to int or double (nil on failure) |

### i/o

| form | description |
|------|-------------|
| `(println a b ...)` | print values separated by spaces, then newline |

## values

nan-boxed 64-bit representation.

| type | range / notes |
|------|---------------|
| nil | singleton |
| bool | `true` / `false` |
| int | 48-bit signed: -2^47 to 2^47-1 |
| double | ieee 754 64-bit |
| string | immutable, interned |
| list | cons cells. nil is the empty list |
| fn | compiled bytecode closure |
| native | built-in c function |
| fiber | lightweight cooperative thread |
| channel | typed communication channel between fibers |

## tail calls

the compiler detects tail position calls and emits `tail-call` instead of
`call`. tail-recursive functions run in constant stack space.

tail position is: the last expression in a function body, or the last expression
in an `if` branch, or the last expression in a `do` block.

```
(def loop (fn [n]
  (if (> n 0)
    (loop (- n 1))    ; tail call, O(1) stack
    (println "done"))))
```
