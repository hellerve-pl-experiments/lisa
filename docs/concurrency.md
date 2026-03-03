# concurrency

lisa has cooperative concurrency via fibers and channels, inspired by csp
(communicating sequential processes).

## fibers

fibers are lightweight threads scheduled cooperatively. only one fiber runs at
a time. a fiber yields when it sends or receives on a channel, or explicitly
calls `yield`.

```
(spawn (fn [] (println "hello from fiber")))
```

`spawn` takes a function and optional arguments. it returns a fiber object.

```
(def f (spawn (fn [x] (println x)) "hi"))
```

the main script runs as the main fiber. spawned fibers run when the main fiber
yields or performs a blocking channel operation.

## channels

channels are the synchronization primitive. `send` and `recv` block the current
fiber until the other side is ready (rendezvous semantics).

```
(def ch (chan))

(spawn (fn []
  (send ch 42)))

(println (recv ch))  ; 42
```

### api

| form | description |
|------|-------------|
| `(chan)` | create a new channel |
| `(send ch val)` | send a value, blocks until received |
| `(recv ch)` | receive a value, blocks until sent |
| `(spawn fn args...)` | spawn a fiber |
| `(yield)` | yield to scheduler (cannot be called from main fiber) |
| `(yield val)` | yield with a value |

## patterns

### producer-consumer

```
(def ch (chan))

(spawn (fn []
  (send ch 1)
  (send ch 2)
  (send ch 0)))  ; sentinel

(def consume (fn [ch]
  (def v (recv ch))
  (if (= v 0) nil
    (do (println v)
        (consume ch)))))

(consume ch)
```

### pipeline

```
(def stage (fn [in out f]
  (spawn (fn []
    (def run (fn []
      (def v (recv in))
      (if (= v 0) (send out 0)
        (do (send out (f v))
            (run)))))
    (run)))))

(def a (chan))
(def b (chan))
(def c (chan))

(stage a b (fn [x] (* x 2)))
(stage b c (fn [x] (+ x 1)))

(send a 10)
(println (recv c))  ; 21
(send a 0)          ; shutdown
```

### fan-out

```
(def work (chan))
(def results (chan))

; spawn N workers
(def spawn-workers (fn [n]
  (if (> n 0)
    (do
      (spawn (fn []
        (def v (recv work))
        (send results (* v v))))
      (spawn-workers (- n 1))))))

(spawn-workers 3)

; send work
(send work 2) (send work 3) (send work 4)

; collect results
(println (recv results))
(println (recv results))
(println (recv results))
```
