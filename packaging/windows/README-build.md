# Building the Windows installer (`BeeSetup.exe`)

This produces a single double-click `BeeSetup-0.1.0.exe` that installs Bee for
non-technical users (PATH entry, `.be`/`.bee` file association with the bee icon,
uninstaller in *Add or remove programs*).

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
# 1. Compile the Windows binary (interpreter only, no LLVM needed).
#    jit_llvm.cpp is #ifdef-guarded, so it compiles to nothing here.
g++ -std=c++17 -O2 -pthread -DBEE_VERSION='\"0.1.0\"' (Get-ChildItem src\*.cpp).FullName -o packaging\windows\bee.exe -pthread

# 2. Build the installer.
cd packaging\windows
iscc bee-setup.iss
```

The result is written to `dist\BeeSetup-0.1.0.exe`.

> If `iscc` isn't found, open `bee-setup.iss` in the **Inno Setup Compiler** GUI
> and press **F9** (Compile) instead.

## What ends up on the user's machine

| Item | Location |
|------|----------|
| `bee.exe` | `C:\Program Files\BeeLang\bin\bee.exe` |
| Examples | `C:\Program Files\BeeLang\examples\` |
| PATH entry | `...\BeeLang\bin` (optional task) |
| `.be` / `.bee` association | bee icon; double-click runs the script (optional task) |
| Uninstaller | *Settings → Apps → BeeLang* |

## VS Code extension

The installer does **not** bundle the VS Code extension (its install path is
per-user and awkward under an elevated installer). Users who want editor
support can copy the `editors/vscode-bee` folder into
`%USERPROFILE%\.vscode\extensions\bee-lang-0.1.0`.
