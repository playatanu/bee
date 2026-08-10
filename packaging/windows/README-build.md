# Building the Windows installer (`bee-<version>-amd64.exe`)

This produces a single double-click `bee-0.3.4-amd64.exe` that installs Bee for
non-technical users (`bee` and `hive` on the PATH, `.be`/`.bee` file association
with the bee icon, uninstaller in *Add or remove programs*).

The JIT backend links LLVM, and on Windows LLVM must be built for the *same*
toolchain as the compiler or the C++ ABIs will not match. The reliable way to
get both is **MSYS2 UCRT64**, which ships a matching g++ and LLVM. (This is what
the release CI uses.)

1. **MSYS2** — https://www.msys2.org. Then, in a **UCRT64** shell:
   ```bash
   pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-llvm
   ```

2. **Inno Setup** — the installer builder:
   ```
   winget install JRSoftware.InnoSetup
   ```

## Steps

From the repository root, in the **MSYS2 UCRT64** shell:

```bash
ver=0.3.4

# 1. Compile the interpreter. It links no LLVM: src/*.cpp includes src/jit.cpp
#    (the dlopen front end) and src/jit_llvm.cpp (guarded to nothing without
#    -DBEE_JIT). -static keeps the .exe self-contained.
g++ -std=c++17 -O2 -static -pthread -DBEE_VERSION="\"$ver\"" \
    src/*.cpp -o packaging/windows/bee.exe

# 2. Compile the LLVM JIT backend as a DLL. bee.exe dlopen's it from its own
#    directory on the first compile; -static and --link-static make it
#    self-contained, and it shares bee.exe's heap through ucrtbase.dll (UCRT).
g++ -std=c++17 -O2 -static -pthread -DBEE_VERSION="\"$ver\"" -DBEE_JIT -fno-rtti \
    -I"$(llvm-config --includedir)" -shared -o packaging/windows/bee_jit.dll \
    src/jit_llvm.cpp \
    $(llvm-config --ldflags --link-static --libs core orcjit native passes --system-libs)

# 3. Compile the package manager (no LLVM).
g++ -std=c++17 -O2 -static -pthread -DHIVE_VERSION="\"$ver\"" \
    src/hive/*.cpp -o packaging/windows/hive.exe

# 4. Compile the binding generator. It loads libclang at run time, so there is
#    nothing to link against here.
g++ -std=c++17 -O2 -pthread -DBEEGEN_VERSION="\"$ver\"" \
    src/beegen/*.cpp -o packaging/windows/beegen.exe

# 5. Build the installer (from a PowerShell prompt, or the Inno Setup GUI).
#    cd packaging\windows ; iscc bee-setup.iss
```

All three `.exe` files **and `bee_jit.dll`** must exist next to `bee-setup.iss`
before step 5 — the installer ships them all, and Inno Setup fails if one is
missing. (If you only want an interpreter-only build to test, skip step 2 and
remove the `bee_jit.dll` line from `bee-setup.iss`; bee falls back to the
interpreter when the backend is absent.)

The result is written to `dist\bee-0.3.4-amd64.exe`.

> If `iscc` isn't found, open `bee-setup.iss` in the **Inno Setup Compiler** GUI
> and press **F9** (Compile) instead.

## What ends up on the user's machine

| Item | Location |
|------|----------|
| `bee.exe` | `C:\Program Files\Bee\bin\bee.exe` |
| `bee_jit.dll` (LLVM JIT backend) | `C:\Program Files\Bee\bin\bee_jit.dll` |
| `hive.exe` (package manager) | `C:\Program Files\Bee\bin\hive.exe` |
| Examples | `C:\Program Files\Bee\examples\` |
| PATH entry | `...\Bee\bin` (optional task) |
| `.be` / `.bee` association | bee icon; double-click runs the script (optional task) |
| Uninstaller | *Settings → Apps → Bee* |

## VS Code extension

The installer does **not** bundle the VS Code extension (its install path is
per-user and awkward under an elevated installer). The extension ships from its
own repository, [beelang-project/vscode-bee](https://github.com/beelang-project/vscode-bee);
users who want editor support install the `.vsix` from its releases with
`code --install-extension`.
