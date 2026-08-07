# Building the Windows installer (`bee-<version>-amd64.exe`)

This produces a single double-click `bee-0.3.0-amd64.exe` that installs Bee for
non-technical users (`bee` and `hive` on the PATH, `.be`/`.bee` file association
with the bee icon, uninstaller in *Add or remove programs*).

You need a **Windows machine** (or a Windows CI runner). Two one-time tools:

1. **A C++ compiler** — MinGW-w64. Easiest:
   ```
   winget install BrechtSanders.WinLibs.POSIX.UCRT
   ```
   Re-open your terminal afterwards so `g++` is on `PATH`.

2. **Inno Setup** — the installer builder:
   ```
   winget install JRSoftware.InnoSetup
   ```

## Steps

From the repository root, in PowerShell:

```powershell
# 1. Compile the interpreter (interpreter only, no LLVM needed).
#    jit_llvm.cpp is #ifdef-guarded, so it compiles to nothing here.
g++ -std=c++17 -O2 -pthread -DBEE_VERSION='\"0.3.0\"' (Get-ChildItem src\*.cpp).FullName -o packaging\windows\bee.exe -pthread

# 2. Compile the package manager (no LLVM either).
g++ -std=c++17 -O2 -pthread -DHIVE_VERSION='\"0.3.0\"' (Get-ChildItem src\hive\*.cpp).FullName -o packaging\windows\hive.exe -pthread

# 3. Compile the binding generator. It loads libclang at run time, so there is
#    nothing to link against here.
g++ -std=c++17 -O2 -pthread -DBEEGEN_VERSION='\"0.3.0\"' (Get-ChildItem src\beegen\*.cpp).FullName -o packaging\windows\beegen.exe -pthread

# 4. Build the installer.
cd packaging\windows
iscc bee-setup.iss
```

All three `.exe` files must exist next to `bee-setup.iss` before step 4 — the
installer ships `beegen.exe` too, and Inno Setup fails if it is missing.

The result is written to `dist\bee-0.3.0-amd64.exe`.

> If `iscc` isn't found, open `bee-setup.iss` in the **Inno Setup Compiler** GUI
> and press **F9** (Compile) instead.

## What ends up on the user's machine

| Item | Location |
|------|----------|
| `bee.exe` | `C:\Program Files\BeeLang\bin\bee.exe` |
| `hive.exe` (package manager) | `C:\Program Files\BeeLang\bin\hive.exe` |
| Examples | `C:\Program Files\BeeLang\examples\` |
| PATH entry | `...\BeeLang\bin` (optional task) |
| `.be` / `.bee` association | bee icon; double-click runs the script (optional task) |
| Uninstaller | *Settings → Apps → BeeLang* |

## VS Code extension

The installer does **not** bundle the VS Code extension (its install path is
per-user and awkward under an elevated installer). The extension ships from its
own repository, [beelang-project/vscode-bee](https://github.com/beelang-project/vscode-bee);
users who want editor support install the `.vsix` from its releases with
`code --install-extension`.
