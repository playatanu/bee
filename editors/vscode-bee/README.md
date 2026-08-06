<div align="center">

<img src="icons/bee.png" width="110" alt="BeeLang logo" />

# BeeLang for VS Code

Language support for **[BeeLang](../../README.md)** — syntax highlighting, smart completions, hovers, and snippets for `.be` and `.bee` files.

[![Version](https://img.shields.io/badge/version-0.1.0-f5b51e.svg?style=flat-square)](package.json)
[![License: MIT](https://img.shields.io/badge/License-MIT-4c9a2a.svg?style=flat-square)](../../LICENSE)
[![VS Code](https://img.shields.io/badge/VS%20Code-%5E1.75-007acc.svg?style=flat-square)](https://code.visualstudio.com/)

</div>

---

## Features

### 🎨 Syntax highlighting
Full TextMate grammar covering keywords, strings (single & double quoted), numbers, both comment styles (`#` and `//`), class and function declarations, built-in functions, constants, and operators.

### 💡 Completions
- **Keywords** — `let`, `fn`, `class`, `if`, `for`, `while`, `return`, `import`, …
- **Built-in functions** — with signatures and inline documentation.
- **Type methods** — `.`-completions for string / list / dict methods (`upper`, `push`, `keys`, …).
- **Local symbols** — functions, classes, `let` variables, parameters, and loop variables declared in the current file.

### 📖 Hovers
Hover any built-in function to see its signature and a short description.

### ✂️ Snippets
Tab-completable snippets for common constructs:

| Prefix | Expands to |
|--------|------------|
| `fn` | function declaration |
| `class` / `classext` | class / class with `extends` |
| `for` / `forc` | `for … in` / C-style `for` |
| `while` | while loop |
| `if` / `ifelse` | conditional blocks |
| `let` | variable declaration |
| `print` | print statement |
| `import` / `from` | module imports |

### ⚙️ Editing conveniences
Auto-closing brackets and quotes, comment toggling (`Ctrl` + `/`), and sensible auto-indentation.

### 🐝 File icons
`.be` and `.bee` files get a bee icon in the Explorer (with a file-icon theme that shows language icons, such as the default **Seti** theme).

---

## Installation

### From source (this repo)

Copy the extension into your VS Code extensions folder and reload:

```bash
# from the repository root
cp -r editors/vscode-bee ~/.vscode/extensions/bee-lang-0.1.0
```

Then reload the window (**Command Palette → Developer: Reload Window**). Open any
`.be` / `.bee` file and language support activates automatically.

> On Windows, copy the folder into `%USERPROFILE%\.vscode\extensions\bee-lang-0.1.0`.

### Develop it (Extension Development Host)

```bash
code editors/vscode-bee
```

Press **F5** to launch a second VS Code window with the extension loaded — ideal
for hacking on the grammar or providers.

### Package a `.vsix` (optional, needs Node)

```bash
npm install -g @vscode/vsce
cd editors/vscode-bee
vsce package                              # → bee-lang-0.1.0.vsix
code --install-extension bee-lang-0.1.0.vsix
```

---

## Requirements

The extension is pure editor tooling — it does **not** bundle the interpreter. To
run programs, install the `bee` interpreter (see the
[project README](../../README.md)) so you can:

```bash
bee path/to/script.bee
```

---

## Known limitations

- **Method completion** after `.` lists the union of string / list / dict methods —
  the extension does no type inference, so pick the one that fits your value.
- **Symbol completion** is a lightweight regex scan of the open file, not a full
  parse. It favors being helpful over being exhaustive.
- Highlighting needs no activation; completions and hovers activate on the first
  `.be` / `.bee` file you open.

---

## Project structure

| File | Purpose |
|------|---------|
| `package.json` | Extension manifest (language, grammar, snippets, icons) |
| `language-configuration.json` | Brackets, comments, auto-closing, indentation |
| `syntaxes/bee.tmLanguage.json` | TextMate grammar (highlighting) |
| `extension.js` | Completion & hover providers |
| `snippets/bee.json` | Code snippets |
| `icons/` | Language / file icons |

No build step — the extension is plain JavaScript.

---

## Release notes

### 0.1.0
Initial release: syntax highlighting, completions (keywords, built-ins, methods,
file symbols), hovers, snippets, editing config, and `.be` / `.bee` file icons.

---

## License

[MIT](../../LICENSE) © 2026 Atanu Debnath. Part of the
[BeeLang](../../README.md) project.
