# internals

## architecture

```
source -> lexer -> parser -> ast -> compiler -> bytecode -> vm
                                                        \-> jit (optional)
```

all files are in `lisa/`. the jit backend uses the cj framework from the parent
project.

## files

| file | purpose |
|------|---------|
| `main.c` | cli entry point |
| `lexer.c/h` | tokenizer |
| `parser.c/h` | s-expression parser, produces ast |
| `ast.c/h` | ast node types and constructors |
| `compiler.c/h` | ast -> bytecode compiler |
| `chunk.c/h` | bytecode chunks and constant pools |
| `vm.c/h` | bytecode interpreter, globals, native functions |
| `value.h` | nan-boxed value representation |
| `object.c/h` | heap objects (string, function, closure, list, etc.) |
| `fiber.c/h` | fiber and channel implementation, scheduler |
| `jit.c/h` | jit compiler (x86-64/arm64 via cj) |
| `debug.c/h` | bytecode disassembler |

## values

nan-boxed in a `uint64_t`. ieee 754 doubles use their natural representation.
tagged values set the quiet-nan bits plus a 3-bit tag:

```
[sign=1][exp=0x7FF][quiet=1][tag 50:48][payload 47:0]

tag 000 = nil
tag 001 = bool    (bit 0 = value)
tag 010 = int     (48-bit sign-extended)
tag 011 = object  (48-bit pointer)
```

## heap objects

all heap objects start with `lisa_obj`: type tag, gc mark bit, next pointer.

| type | struct | notes |
|------|--------|-------|
| string | `lisa_obj_string` | interned, flexible array for chars |
| function | `lisa_obj_function` | bytecode chunk, arity, name, jit pointers |
| closure | `lisa_obj_closure` | function + captured upvalues |
| upvalue | `lisa_obj_upvalue` | location pointer, closed value, linked list |
| list | `lisa_obj_list` | cons cell: car + cdr |
| native | `lisa_obj_native` | c function pointer, name, arity (-1 = variadic) |
| fiber | `lisa_fiber` | own stack, frames, scheduler state |
| channel | `lisa_channel` | send/recv queues for rendezvous |

## opcodes

21 opcodes. encoding is 1-byte opcode + 0-2 bytes operand.

| opcode | operands | description |
|--------|----------|-------------|
| `CONSTANT` | idx | push constants[idx] |
| `NIL` | | push nil |
| `TRUE` | | push true |
| `FALSE` | | push false |
| `POP` | | pop top |
| `GET_LOCAL` | slot | push stack[base+slot] |
| `SET_LOCAL` | slot | stack[base+slot] = peek |
| `GET_UPVALUE` | idx | push upvalue value |
| `SET_UPVALUE` | idx | set upvalue value |
| `GET_GLOBAL` | idx | push global by name |
| `DEF_GLOBAL` | idx | define global by name |
| `ADD` `SUB` `MUL` `DIV` `MOD` | | binary arithmetic |
| `NEGATE` | | unary negate |
| `EQUAL` `NOT_EQUAL` | | equality |
| `LESS` `LESS_EQUAL` `GREATER` `GREATER_EQUAL` | | comparison |
| `NOT` | | logical not |
| `JUMP` | lo, hi | ip += offset |
| `JUMP_IF_FALSE` | lo, hi | conditional jump, pops condition |
| `LOOP` | lo, hi | ip -= offset |
| `CLOSURE` | idx, then pairs | create closure with upvalue descriptors |
| `CALL` | argc | call function |
| `TAIL_CALL` | argc | tail call (reuses frame) |
| `RETURN` | | return top of stack |
| `CLOSE_UPVALUE` | | close upvalue at stack top, pop |
| `CLOSE_UPVALUES_AT` | slot | close upvalues at slot and above (no pop) |
| `CONS` | | push cons(pop2, pop1) |
| `CAR` | | push car(pop) |
| `CDR` | | push cdr(pop) |
| `LIST` | n | pop n items, build list |
| `PRINTLN` | argc | print argc values with spaces, newline |

## gc

mark-and-sweep. roots: stack, globals, open upvalues, all live fibers. triggers
when `bytes_allocated` exceeds `next_gc` (doubles after each collection).

string interning uses a hash table for deduplication.

## jit

optional. compiles bytecode functions to native code on first call. uses the cj
framework for code emission. registers:

- frame base pointer (slots)
- stack top pointer
- 4 register cache slots to defer stack pushes
- vm pointer for runtime calls (gc, upvalue close, fiber ops)

the jit calls back into c for complex operations: gc allocation, upvalue
capture/close, fiber scheduling, native function dispatch.

disable with `--no-jit` for debugging or on unsupported platforms.

## scoping

variable resolution: local -> upvalue -> global.

`def` at top level emits `DEF_GLOBAL`. inside a function, it emits
`NIL` + `add_local` + compile value + `SET_LOCAL` + `POP`. the nil initializes
the slot, `add_local` registers the name (enabling self-referencing closures),
and `SET_LOCAL` writes the real value.

`let` creates locals in a new scope. `end_scope_with_result` handles cleanup:
it closes captured upvalues first (`CLOSE_UPVALUES_AT`), then moves the result
to the first local's slot (`SET_LOCAL`), then pops all locals.
