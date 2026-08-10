# Get started

This page takes you from nothing installed to running your first Bee program.
No prior experience needed.

## 1. Install Bee

[Download the installer](download.md) for your system and run it. It sets up
two commands you'll use:

- `bee` - runs your programs.
- `hive` - installs add-on packages (optional, for later).

To check it worked, open a terminal and run:

```bash
bee --version
```

If you see a version number, you're ready.

## 2. Write your first program

Open any text editor. Create a file called `hello.bee` and type:

```
print("Hello, world!")
```

Save it.

## 3. Run it

In a terminal, in the same folder as the file, run:

```bash
bee hello.bee
```

You should see:

```
Hello, world!
```

That's the whole workflow - edit the file, run `bee`, see the result.

## A slightly bigger example

Try this to get a feel for the language:

```
let names = ["Ada", "Alan", "Grace"]

for name in names {
    print("Hello, " + name + "!")
}
```

Running it prints one greeting per line. You just used a **list** and a
**loop** - two of the building blocks you'll use all the time.

## Keep learning

- [Language guide](language.md) - the full tour: variables, functions,
  classes, and more.
- [Packages](hive.md) - install ready-made code with Hive.
- [Editor](editor.md) - set up VS Code for a nicer experience.
