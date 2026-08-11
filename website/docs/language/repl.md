# The REPL

Run `bee` with no arguments for an interactive session. A bare expression prints
its value; definitions persist across lines, and an open block keeps prompting
with `...` until it closes:

```
$ bee
bee 0.3.2 - interactive session
type an expression to see its value; 'exit' or Ctrl-D to quit
>>> 1 + 1
2
>>> let x = 10
>>> f"x is {x}"
"x is 10"
>>> fn fact(n) {
...     if n <= 1 { return 1 }
...     return n * fact(n - 1)
... }
>>> fact(10)
3628800
```

An error prints and the session continues. Leave with `exit` or Ctrl-D.

Two more ways in, for scripts and pipelines:

```bash
bee -e 'print(f"{2 * 21}")'    # run one line and exit
bee < script.bee               # read the program from stdin
echo 'print("hi")' | bee       # same, in a pipeline
```
