---
description: >-
  The Bee VS Code extension: syntax highlighting, completions, hovers, and snippets for .be and .bee files.
---

# VS Code Extension

Language support for Bee - **syntax highlighting, smart completions, hovers,
and snippets** for `.be` and `.bee` files. It's pure editor tooling and does
**not** bundle the interpreter.

The extension lives in its own repository,
[beelang-project/vscode-bee](https://github.com/beelang-project/vscode-bee),
and is versioned independently of the interpreter.

## Features

### Syntax highlighting
A full TextMate grammar covering keywords, strings (single & double quoted),
numbers, both comment styles (`#` and `//`), class and function declarations,
built-in functions, constants, and operators. It also highlights `hive.json`
and `hive.lock` package files.

### Completions
- **Keywords** - `let`, `fn`, `class`, `if`, `for`, `while`, `return`, `import`, …
- **Built-in functions** - with signatures and inline documentation.
- **Type methods** - `.`-completions for string / list / dict methods
  (`upper`, `push`, `keys`, …).
- **Local symbols** - functions, classes, `let` variables, parameters, and loop
  variables declared in the current file.

### Hovers
Hover any built-in function to see its signature and a short description.

### Snippets
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

### Editing conveniences
Auto-closing brackets and quotes, comment toggling (++ctrl+slash++), and
sensible auto-indentation.

### File icons
`.be` and `.bee` files get a bee icon in the Explorer (with a file-icon theme
that shows language icons, such as the default **Seti** theme).

## Installation

=== "From a released `.vsix`"

    Download the `.vsix` from the
    [latest release](https://github.com/beelang-project/vscode-bee/releases) and
    install it:

    ```bash
    code --install-extension vscode-bee-<version>.vsix
    ```

=== "From source"

    Copy the repository into your VS Code extensions folder and reload:

    ```bash
    git clone https://github.com/beelang-project/vscode-bee.git \
        ~/.vscode/extensions/vscode-bee
    ```

    Then reload the window (**Command Palette → Developer: Reload Window**).
    On Windows, clone into `%USERPROFILE%\.vscode\extensions\vscode-bee`.

=== "Package a `.vsix` yourself"

    Needs Node:

    ```bash
    npm install -g @vscode/vsce
    vsce package                                   # → vscode-bee-<version>.vsix
    code --install-extension vscode-bee-<version>.vsix
    ```

Open any `.be` / `.bee` file and language support activates automatically.

!!! info "You still need the interpreter"
    The extension is editor tooling only. To run programs, install the `bee`
    interpreter (see [Getting Started](getting-started.md)) so you can:

    ```bash
    bee path/to/script.bee
    ```

## Known limitations

- **Method completion** after `.` lists the union of string / list / dict
  methods - the extension does no type inference, so pick the one that fits your
  value.
- **Symbol completion** is a lightweight regex scan of the open file, not a full
  parse. It favors being helpful over being exhaustive.
- Highlighting needs no activation; completions and hovers activate on the first
  `.be` / `.bee` file you open.

## Develop it

Open the extension repo in VS Code and press ++f5++ to launch a second window
(the *Extension Development Host*) with the extension loaded - ideal for hacking
on the grammar or the completion/hover providers. There's no build step; the
extension is plain JavaScript.

| File | Purpose |
|------|---------|
| `package.json` | Extension manifest (language, grammar, snippets, icons) |
| `language-configuration.json` | Brackets, comments, auto-closing, indentation |
| `syntaxes/bee.tmLanguage.json` | TextMate grammar (highlighting) |
| `extension.js` | Completion & hover providers |
| `snippets/bee.json` | Code snippets |
| `icons/` | Language / file icons |
