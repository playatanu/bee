# Download

Bee is free and open source. Pick your platform below - each installer sets up
both `bee` (the language) and `hive` (its package manager).

<div class="bee-downloads">
  <a class="bee-dl" href="https://github.com/beelang-project/bee/releases/latest">
    <span class="bee-dl__os">Windows</span>
    <span class="bee-dl__file">bee-&lt;version&gt;-amd64.exe</span>
    <span class="bee-dl__hint">Run the installer. You can then double-click any .be / .bee file to run it.</span>
  </a>
  <a class="bee-dl" href="https://github.com/beelang-project/bee/releases/latest">
    <span class="bee-dl__os">Debian / Ubuntu</span>
    <span class="bee-dl__file">bee-&lt;version&gt;-amd64.deb</span>
    <span class="bee-dl__hint">Double-click to open your software centre, or install it from a terminal.</span>
  </a>
</div>

After installing, open a terminal and check it works:

```bash
bee --version
```

New here? Head to [Get started](getting-started.md) to write and run your first
program.

## Build it yourself

Prefer to build from source? You'll need a C++17 compiler, `make`, and
(optionally) LLVM for the speed boost:

```bash
sudo apt install llvm-18-dev   # optional, enables the JIT
make                           # builds bee, hive and beegen
```

Without LLVM, `make` still produces a working `bee` - just a little slower.
