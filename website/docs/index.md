---
title: The Bee Scripting Language
description: >-
  Bee is a small, friendly scripting language that's easy to read and quick to
  learn, with a built-in native (LLVM) JIT that keeps tight loops fast.
hide:
  - navigation
  - toc
---

<div class="bee-hero" markdown>

![Bee logo](assets/bee.png)

# Bee

<p class="tagline">A friendly scripting language with a built-in native (LLVM) JIT.</p>

[Download](download.md){ .md-button .md-button--primary }
[Get started](getting-started.md){ .md-button }
[VS Code extension](https://marketplace.visualstudio.com/items?itemName=playatanu.vscode-bee){ .md-button }

</div>

## Hello, Bee

Save a file called `hello.bee`:

```
print("Hello, world!")
```

Run it:

```bash
bee hello.bee
```

That's the whole loop: write a `.bee` file, run it with `bee`. No setup, no
build step.

## Get started in three steps

<div class="bee-steps" markdown>

<div class="bee-step" markdown>
### 1. Install
[Download Bee](download.md) for Windows or Linux. It's a one-click installer.
</div>

<div class="bee-step" markdown>
### 2. Write
Open any text editor and save a file ending in `.bee`.
</div>

<div class="bee-step" markdown>
### 3. Run
Type `bee yourfile.bee` in a terminal. That's it.
</div>

</div>

## What Bee is good at

<div class="bee-grid" markdown>

<div class="bee-card" markdown>
### Easy to read
Clean, familiar syntax. If you've programmed before, Bee will feel natural. If
you haven't, it's a gentle place to start.
</div>

<div class="bee-card" markdown>
### Batteries included
Text, lists, files, and time are built in. Need more? The [Hive](hive.md)
package manager installs it in one command.
</div>

<div class="bee-card" markdown>
### Fast when it matters
Bee quietly speeds up heavy number work in the background, so your programs
stay quick with no effort from you.
</div>

</div>

## Where to next

- [Get started](getting-started.md) - install Bee and run your first program.
- [Language guide](language.md) - learn the language, one topic at a time.
- [Packages](hive.md) - find and install ready-made code with Hive.
- [Editor](editor.md) - syntax highlighting and hints in VS Code.
