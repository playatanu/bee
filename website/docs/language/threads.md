# Threads

Bee has a **global interpreter lock (GIL)**, like CPython: threads run
concurrently and share data safely, but only one runs Bee code at a time. This
is ideal for I/O-bound work (network, files, `sleep`, `exec`) - the lock is
released during those blocking calls so other threads make progress. It does
**not** give CPU parallelism.

| Function | Description |
|----------|-------------|
| `spawn(fn[, ...args])` | Start `fn(...args)` on a new thread; returns a handle. |
| `join(handle)` | Wait for the thread and return its result (re-raises a thrown error). |

```
fn download(url) {
    # ... a blocking call releases the lock, so peers run meanwhile ...
    return exec("curl -s " + url).output
}

let a = spawn(download, "http://example.com/a")
let b = spawn(download, "http://example.com/b")
let ra = join(a)
let rb = join(b)      # both downloads overlapped
```

Because the GIL serializes Bee execution, shared lists/dicts/objects don't
corrupt - updates between blocking points are effectively atomic:

```
let total = [0]
fn add_up() { for i in range(100000) { total[0] = total[0] + 1 } }
let ts = []
for i in range(4) { ts.push(spawn(add_up)) }
for t in ts { join(t) }
print(total[0])       # exactly 400000 - no lost updates
```

Any threads you don't `join` are joined automatically when the program ends.
Errors thrown inside a thread are re-raised by `join`, so wrap it in `try`:

```
fn risky() { throw "nope" }
let t = spawn(risky)
try { join(t) } catch (e) { print("thread failed:", e) }
```

> **Note:** there are no anonymous/lambda functions yet, so pass a *named*
> function to `spawn` (`spawn(worker, arg)`), not an inline one.
