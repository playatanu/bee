# Bee examples

A topic-by-topic tour of the language. Run any file with the interpreter:

```bash
bee examples/01_hello.bee
```

Work through them in order, or jump to a topic:

| #  | File                     | Topic |
|----|--------------------------|-------|
| 01 | `01_hello.bee`           | Your first program |
| 02 | `02_values.bee`          | Numbers, strings, booleans, nil; `let` and `const` |
| 03 | `03_operators.bee`       | Arithmetic, comparison, logical, bitwise, ternary |
| 04 | `04_strings.bee`         | String methods, f-strings, indexing, slicing |
| 05 | `05_collections.bee`     | Lists, dicts, comprehensions, slicing |
| 06 | `06_control_flow.bee`    | `if`/`else if`/`else`, `while`, `for`, `break`/`continue` |
| 07 | `07_match.bee`           | Pattern matching with `match` |
| 08 | `08_functions.bee`       | Functions, defaults, `...rest`, closures |
| 09 | `09_classes.bee`         | Classes, inheritance, `this`/`super`, `str()` |
| 10 | `10_errors.bee`          | `try`/`catch`/`finally`, `throw` |
| 11 | `11_numeric_types.bee`   | Sized numeric types (`i8`…`u64`, `f16`/`f32`/`f64`) |
| 12 | `12_modules.bee`         | Importing modules (uses `mathutil.bee`) |
| 13 | `13_threads.bee`         | Threads with `spawn`/`join` |
| 14 | `14_system.bee`          | Files, time, randomness, environment |
| 15 | `15_input.bee`           | Reading `input()` and command-line `args()` |
| 16 | `16_compiling.bee`       | Compiling to a native binary with `beec` |
| 17 | `17_fizzbuzz.bee`        | A complete small program |

`mathutil.bee` is a library module imported by `12_modules.bee` — not a
standalone program.

Most of these also compile to a standalone native executable with `beec`, the
ahead-of-time compiler:

```bash
beec examples/16_compiling.bee -o wordcount && ./wordcount
```

## Adding an example

Drop a `NN_topic.bee` file in this directory, keeping the numeric prefix in
sequence, and add a row to the table above. Keep a program's output
deterministic (no clock or randomness) so it stays in step across the
interpreter, the register VM, and the AOT compiler — the differential test
runs every example on both engines and checks they agree. Programs that
genuinely need the outside world (`14_system.bee`) or stdin (`15_input.bee`)
are the two exceptions and are skipped by that test.
