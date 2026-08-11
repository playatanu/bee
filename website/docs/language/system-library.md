# System library

These built-ins reach outside the program: the filesystem, the clock, the OS
environment, and external processes. They're always available (no import).

### File I/O
| Function | Description |
|----------|-------------|
| `read_file(path)` | Whole file as a string. |
| `read_lines(path)` | List of lines (newlines stripped). |
| `write_file(path, text)` | Write `text`, truncating the file. |
| `append_file(path, text)` | Append `text` to the file. |
| `file_exists(path)` | `true` if the path exists. |
| `remove_file(path)` | Delete a file; returns whether it was removed. |
| `make_dir(path)` | Create a directory (and parents); returns whether it was created. |
| `list_dir(path)` | List of entry names in a directory. |

```
write_file("notes.txt", "hello\nworld\n")
print(read_lines("notes.txt"))     # ["hello", "world"]
if file_exists("notes.txt") { remove_file("notes.txt") }
```

### Time & date
| Function | Description |
|----------|-------------|
| `clock()` | Monotonic seconds - use for measuring elapsed time. |
| `time()` | Seconds since the Unix epoch (with fraction). |
| `now()` | Local date/time as a dict: `year, month, day, hour, minute, second, weekday, yearday`. |
| `format_time(fmt[, epoch])` | Format with `strftime` codes (e.g. `"%Y-%m-%d %H:%M"`). |
| `sleep(seconds)` | Pause; releases the lock so other threads run. |

```
let t0 = clock()
heavy_work()
print("took", clock() - t0, "seconds")
print(format_time("%Y-%m-%d"))     # 2026-08-05
```

### Random
| Function | Description |
|----------|-------------|
| `random()` | Float in `[0, 1)`. |
| `random_int(a, b)` | Integer in `[a, b]` inclusive. |
| `random_range(a, b)` | Float in `[a, b)`. |
| `random_choice(list)` | A random element. |
| `random_seed(n)` | Seed the generator (same seed ⇒ same sequence). |

```
random_seed(42)
print(random_int(1, 6))            # a dice roll, reproducible
print(random_choice(["red", "green", "blue"]))
```

### Environment & arguments
| Function | Description |
|----------|-------------|
| `env(name[, default])` | Environment variable, or `default` / `nil`. |
| `set_env(name, value)` | Set an environment variable. |
| `args()` | List of command-line arguments after the script path. |

```
./bee script.bee alpha beta      # args() == ["alpha", "beta"]
print(env("HOME"))
print(env("MISSING", "n/a"))
```

### Processes
| Function | Description |
|----------|-------------|
| `exec(cmd)` | Run `cmd` in the shell; returns `{"code": exit_status, "output": stdout}`. |

```
let r = exec("ls -1")
if r.code == 0 {
    for line in r.output.trim().split("\n") { print(line) }
}
```
