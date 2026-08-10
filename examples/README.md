# Bee examples

A topic-by-topic tour of the language. Run any file with the `bee` interpreter:

```bash
bee examples/01_hello.bee
```

Work through them in order, or jump to the topic you need:

| #  | File                                | Topic |
|----|-------------------------------------|-------|
| 01 | `01_hello.bee`                      | Your first program |
| 02 | `02_variables_and_operators.bee`    | Variables, types, and operators |
| 03 | `03_strings.bee`                    | String operations |
| 04 | `04_collections.bee`                | Lists and dicts |
| 05 | `05_control_flow.bee`               | `if` / `while` / `for`, `break` / `continue` |
| 06 | `06_functions.bee`                  | Functions and closures |
| 07 | `07_classes.bee`                    | Classes and inheritance |
| 08 | `08_modules.bee`                    | Importing modules |
| 09 | `09_error_handling.bee`             | `try` / `catch` / `finally`, `throw` |
| 10 | `10_system.bee`                     | Files, time, randomness, environment, processes |
| 11 | `11_threads.bee`                    | Threads and the GIL |
| 12 | `12_fizzbuzz.bee`                   | A complete small program |
| 13 | `13_benchmark.bee`                  | A numeric benchmark for the native JIT |
| 14 | `14_input.bee`                      | Reading user input (`input()`) and command-line args (`args()`) |

`mathutil.bee` is a library module imported by `08_modules.bee` — not a
standalone program.

[`hive-demo/`](hive-demo/) is a walkthrough of the package manager: pack a
package, install it, import it. It needs no registry and no network.

For the complete language documentation, see the
[language reference](../docs/LANGUAGE.md).
