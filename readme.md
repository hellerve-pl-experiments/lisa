# lisa

wherein we vibe-code a jitted green-threaded lisp usng [cj](https://github.com/hellerve-pl-experiments/cj).

no line outside of this readme was touched by a human. read
[docs/conversation.html](docs/conversation.html) if you want to see how the
language was produced.

## usage

code looks like clojure, just more basic.

```
bin/lisa -e '(def fib (fn [n] (if (<= n 1) n (+ (fib (- n 1)) (fib (- n 2)))))) (println (fib 25))'
```

features bytecode vm with whole function jit and tco, and greenthreads with
channels:

```
; Two fibers play ping-pong over a pair of channels.

(def ping-ch (chan))
(def pong-ch (chan))

(spawn (fn []
  (def ping-loop (fn [n]
    (if (> n 0)
      (do
        (send ping-ch "ping")
        (recv pong-ch)
        (ping-loop (- n 1))))))
  (ping-loop 5)
  (send ping-ch "done")))

(def pong-loop (fn []
  (def msg (recv ping-ch))
  (if (= msg "done")
    (println "finished after 5 rounds")
    (do
      (println msg)
      (send pong-ch "pong")
      (pong-loop)))))
(pong-loop)
```

<hr/>

have fun!
