# cli usage

## running programs

```
lisa <file.lisa>
lisa -e "<expression>"
```

## flags

| flag | description |
|------|-------------|
| `--no-jit` | disable jit compilation, use bytecode interpreter only |

`--no-jit` must come before the file or `-e` argument.

## examples

```
lisa examples/json.lisa
lisa examples/sieve.lisa
lisa -e "(println (+ 1 2))"
lisa --no-jit -e "(println (* 6 7))"
```

## exit codes

| code | meaning |
|------|---------|
| 0 | success |
| 64 | usage error (bad arguments) |
| 65 | compile error |
| 70 | runtime error |
| 74 | file not found |
