# Hive demo — building, installing and importing a package

A two-directory walkthrough of [Hive](../../docs/HIVE.md), Bee's package
manager. No registry and no network needed: you pack a package locally and
install the archive straight into the app.

```
hive-demo/
├── greet/        a package: hive.json + init.bee
└── app/          a program that imports it
```

## 1. Pack the package

```bash
cd examples/hive-demo/greet
hive pack
```

That writes `greet-0.1.0.pkg` and prints its SHA-256. The `.pkg` is a compact,
self-contained container (see the [format notes](../../docs/HIVE.md#the-pkg-package-format)).

## 2. Install it into the app

```bash
cd ../app
hive install ../greet/greet-0.1.0.pkg
```

Hive unpacks it to `app/hive_modules/greet/`, records the install in
`hive_modules/.hive/greet.json`, adds `"greet": "^0.1.0"` to `app/hive.json`,
and writes `app/hive.lock`.

```bash
hive list
```

## 3. Run it

```bash
bee main.bee
```

```
Hello, Bee!
Bonjour, monde!
Namaste, duniya!
```

`import greet` found the package because `bee` searches `hive_modules/` in the
script's directory and every directory above it. Nothing had to be configured.

## 4. Compile it to a native binary (optional)

The app depends on an installed package, but it still compiles to a standalone
executable with [`beec`](../../docs/COMPILING.md) — the package is resolved from
`hive_modules/` and compiled straight into the binary:

```bash
beec main.bee -o greet-demo
./greet-demo
```

```
Hello, Bee!
Bonjour, monde!
Namaste, duniya!
```

No interpreter and no `hive_modules/` are needed to run `greet-demo` — the
dependency is baked in.

## 5. Clean up

```bash
hive uninstall greet
```

That deletes `hive_modules/greet/` and drops the dependency from `hive.json`.

---

Real installs come from a registry instead — `hive install greet` — which is
just static files on any web server. See the
[Hive guide](../../docs/HIVE.md#running-a-registry).
