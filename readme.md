# lisa

wherein we vibe-code a jitted lisp usng [cj](https://github.com/hellerve-pl-experiments/cj).

## usage

code looks like clojure, just more basic.

```
bin/lisa -e '(def fib (fn [n] (if (<= n 1) n (+ (fib (- n 1)) (fib (- n 2)))))) (println (fib 25))'
```

features bytecode vm with whole function jit and tco.

<hr/>

have fun!
