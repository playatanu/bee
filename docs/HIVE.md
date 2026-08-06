# Hive — the BeeLang package manager

Hive is to BeeLang what pip is to Python: one command that fetches a package,
puts it somewhere the interpreter looks, and remembers what you installed.

```bash
hive install strutil            # from the registry
hive install strutil@1.2.0      # a specific version
hive install ./strutil-1.2.0.hive   # from a local archive
hive install                    # everything in hive.json
```

After that, `bee` just finds it:

```bee
import strutil

print(strutil.titlecase("hello world"))
```

Hive ships alongside `bee` — the `.deb` and the Windows installer both put
`hive` on your `PATH`. Building from source produces both binaries:

```bash
make            # builds ./bee and ./hive
make test       # end-to-end tests for hive and module resolution
```

---

## Contents

- [Where packages go](#where-packages-go)
- [Commands](#commands)
- [hive.json](#hivejson)
- [hive.lock](#hivelock)
- [Version constraints](#version-constraints)
- [Writing and publishing a package](#writing-and-publishing-a-package)
- [The .hive archive format](#the-hive-archive-format)
- [Running a registry](#running-a-registry)
- [Configuration](#configuration)
- [Security](#security)

---

## Where packages go

A project keeps its packages next to its code, in `hive_modules/`:

```
my-app/
├── hive.json               what this project depends on
├── hive.lock               exact versions + hashes that were installed
├── main.bee
└── hive_modules/
    ├── .hive/              hive's own install records — don't edit
    │   ├── greet.json
    │   └── logger.json
    ├── greet/
    │   ├── hive.json
    │   └── init.bee
    └── logger/
        ├── hive.json
        └── init.bee
```

`hive install -g` installs into a shared library instead — `~/.hive/lib`
(`%USERPROFILE%\.hive\lib` on Windows), which every script on the machine can
import. Use it for tools you want everywhere, and the project-local default for
anything a project actually depends on.

**How `bee` finds a module.** `import name` searches, in order:

1. the importing file's own directory
2. its sibling `lib/` directory
3. `hive_modules/` in that directory and in every directory above it
4. each entry of `$BEE_PATH` (`;`-separated on Windows, `:` elsewhere)
5. the global library, `$HIVE_HOME/lib` (default `~/.hive/lib`)

Local code wins over an installed package of the same name, so a project can
always override something it installed. Searching `hive_modules/` all the way up
the tree is what lets an installed package import *its* dependencies from the
one flat directory, with no nesting and no duplicate copies.

At each root, `import greet` tries `greet.bee`, `greet.be`, `greet`, and then
`greet/` as a package directory — whose entry module is the `"main"` from its
`hive.json`, falling back to `init.bee`, `init.be`, `greet.bee`, `main.bee`.

`import pkg.submodule` maps to `pkg/submodule.bee`, so a package can expose more
than one module.

---

## Commands

| Command | What it does |
|---|---|
| `hive install` | install every dependency in `hive.json` |
| `hive install <name>` | install the newest version that fits, and save it to `hive.json` |
| `hive install <name>@<constraint>` | install a specific version or range |
| `hive install <file.hive>` | install a local archive; its dependencies still come from the registry |
| `hive uninstall <name>...` | delete the package and drop it from `hive.json` |
| `hive list` | show what's installed, with versions |
| `hive info <name>` | a package's versions, dependencies and description |
| `hive search <query>` | search the registry index |
| `hive init [dir]` | write a starter `hive.json` (and an `init.bee` if there's none) |
| `hive pack [dir]` | build a `.hive` archive from a package directory |
| `hive cache dir` / `hive cache clean` | show or clear the download cache |

`add`/`i`, `remove`/`rm`, `ls`, `show` and `build` work as aliases.

### Options

| Option | Effect |
|---|---|
| `-g`, `--global` | act on `~/.hive/lib` instead of `./hive_modules` |
| `-u`, `--update` | ignore `hive.lock` pins and take the newest match |
| `--offline` | never touch the network; install from the lockfile and cache |
| `--registry <url>` | use this registry for one command |
| `--no-save` | install without recording it in `hive.json` |
| `--force` | overwrite a directory Hive didn't create |
| `-o`, `--output <file>` | where `hive pack` writes the archive |
| `-C`, `--dir <dir>` | treat this directory as the project root |
| `-q`, `--quiet` | print only errors |

Commands work from anywhere inside a project: Hive walks up to the nearest
directory with a `hive.json`, the way `git` finds its repository.

---

## hive.json

The manifest describes a package — or, in an application, just what it needs.

```json
{
  "name": "strutil",
  "version": "1.2.0",
  "description": "String helpers for BeeLang",
  "author": "Atanu Debnath",
  "license": "MIT",
  "homepage": "https://github.com/you/strutil",
  "repository": "https://github.com/you/strutil",
  "keywords": ["strings", "text"],
  "main": "init.bee",
  "dependencies": {
    "logger": "^1.0.0"
  },
  "files": ["init.bee", "src"],
  "exclude": ["tests", "notes.md"]
}
```

| Field | Meaning |
|---|---|
| `name` | lowercase letters, digits, `-` and `_`, starting with a letter. It becomes a directory name *and* what you type in `import`, so it has to work as both. Required to publish. |
| `version` | `MAJOR.MINOR.PATCH`, optionally `-prerelease`. Required to publish. |
| `main` | the module `import <name>` loads. Defaults to `init.bee`. |
| `dependencies` | package name → [version constraint](#version-constraints) |
| `files` | optional whitelist for `hive pack`; a directory name includes everything under it. `hive.json` is always included. |
| `exclude` | optional prune list for `hive pack` |

An application's `hive.json` can be little more than `{"dependencies": {...}}` —
`name` and `version` are only required for something you intend to publish.

Unknown fields are preserved when Hive rewrites the file, so you can keep your
own metadata in there.

---

## hive.lock

`hive install` writes `hive.lock` with the exact version, URL and SHA-256 of
everything it resolved:

```json
{
  "lockVersion": 1,
  "packages": {
    "greet": {
      "version": "1.2.0",
      "url": "https://packages.beelang.dev/files/greet-1.2.0.hive",
      "sha256": "bbfaecb3…",
      "dependencies": { "logger": "^1.0.0" }
    }
  }
}
```

Commit it. A later `hive install` reuses those pins whenever they still satisfy
your constraints, so everyone on the project — and CI — installs the same bytes.
Because the lockfile carries the URL and hash, `hive install --offline` needs no
registry at all: it installs straight from the cache. `hive install -u` ignores
the pins and moves to the newest matching versions.

---

## Version constraints

| Constraint | Matches |
|---|---|
| `1.2.3` or `=1.2.3` | exactly that version |
| `^1.2.3` | `>=1.2.3` without changing the leftmost non-zero part — `1.9.0` yes, `2.0.0` no |
| `~1.2.3` | patch updates only — `1.2.9` yes, `1.3.0` no |
| `>=1.2.3`, `>1.2.3`, `<=2.0.0`, `<2.0.0` | the obvious thing |
| `*`, `latest`, or omitted | any version |
| `>=1.2.0, <2.0.0` | comma- or space-separated terms must *all* hold |

`hive install greet` records `^<installed version>`, which is the usual "keep up
with compatible releases" default. A prerelease sorts before its release, so
`1.0.0-rc1 < 1.0.0`.

Hive picks the highest non-yanked version satisfying every constraint on a
package. When nothing satisfies them all, it says which constraint came from
where instead of guessing:

```
hive: no version of 'logger' satisfies:
       ^1.0.0 (from greet@1.2.0)
       >=2.0.0 (requested)
```

---

## Writing and publishing a package

```bash
mkdir strutil && cd strutil
hive init                 # writes hive.json and a starter init.bee
$EDITOR init.bee
hive pack                 # -> strutil-1.2.0.hive, and prints its sha256
```

`init.bee` is the package's public surface. Everything it defines is what
`import strutil` exposes; names starting with `_` stay private to the package
(`from strutil import *` skips them).

```bee
# init.bee
fn titlecase(s) { ... }

fn _helper(s) { ... }     # private
```

Test it against a real project before publishing:

```bash
cd ../my-app
hive install ../strutil/strutil-1.2.0.hive
bee main.bee
```

To publish, upload the `.hive` file somewhere your registry can serve it and add
a version entry with its `sha256` (which `hive pack` prints) to the registry
metadata — see below.

---

## The .hive archive format

A `.hive` file is a plain, uncompressed container: no zip, no tar, no
third-party library on either side.

```
HIVE1\n
<header-byte-length>\n
{"format":1,"manifest":{…},"files":[{"path":"init.bee","size":188,"sha256":"…"}]}\n
<the file bytes, concatenated in `files` order>
```

So `head -2 pkg.hive` tells you what's inside. Every file carries its own
SHA-256, and the reader rejects the archive if a hash doesn't match, if bytes
are missing, or if there are unexpected bytes at the end — a corrupt download
fails loudly instead of installing half a package.

---

## Running a registry

A registry is **static files**. GitHub Pages, S3, any web server, or a directory
on disk all work; there's nothing to run.

```
<registry>/
├── index.json                    the catalogue `hive search` reads
├── packages/
│   ├── strutil.json              per-package metadata `hive install` reads
│   └── logger.json
└── files/
    ├── strutil-1.2.0.hive
    └── logger-1.0.0.hive
```

`packages/<name>.json`:

```json
{
  "name": "strutil",
  "description": "String helpers for BeeLang",
  "homepage": "https://github.com/you/strutil",
  "versions": {
    "1.1.0": {
      "url": "files/strutil-1.1.0.hive",
      "sha256": "9f2c…",
      "dependencies": {},
      "yanked": true
    },
    "1.2.0": {
      "url": "files/strutil-1.2.0.hive",
      "sha256": "bbfa…",
      "dependencies": { "logger": "^1.0.0" }
    }
  }
}
```

`url` may be absolute or relative to the registry root — relative keeps a mirror
working when you copy it elsewhere. `yanked: true` hides a version from new
resolutions without breaking a lockfile that already names it.

`index.json`, used only by `hive search`:

```json
{"packages": [
  {"name": "strutil", "version": "1.2.0", "description": "String helpers for BeeLang"},
  {"name": "logger",  "version": "1.0.0", "description": "Tiny logging helpers"}
]}
```

Point Hive at your own with `--registry`, `HIVE_REGISTRY`, or
`~/.hive/config.json`. A filesystem path works too, which is how the test suite
runs without a network:

```bash
hive install strutil --registry ./my-registry
hive install strutil --registry file:///srv/hive
```

---

## Configuration

| Setting | Where |
|---|---|
| Registry | `--registry`, else `$HIVE_REGISTRY`, else `~/.hive/config.json`, else `https://packages.beelang.dev` |
| Hive home | `$HIVE_HOME`, default `~/.hive` — holds `lib/`, `cache/`, `config.json` |
| Extra module roots for `bee` | `$BEE_PATH` |

```json
// ~/.hive/config.json
{ "registry": "https://packages.example.com" }
```

Downloads are cached in `$HIVE_HOME/cache`, keyed by content hash — so a cache
hit verifies itself, and two packages shipping identical bytes share one entry.

---

## Security

Hive is careful about the parts that install untrusted bytes:

- **Hashes are checked, not trusted.** A download whose SHA-256 doesn't match
  the registry's is discarded, never unpacked. Within an archive, every file has
  its own hash.
- **Archives can't escape their directory.** Absolute paths, `..`, drive
  letters and backslashes are rejected outright, so a crafted `.hive` can't
  write outside `hive_modules/<name>/`.
- **Your files aren't collateral.** Hive records what it installed and refuses
  to delete a directory it didn't create unless you pass `--force`.
- **URLs aren't shell commands.** Downloads shell out to `curl` or `wget`, and a
  URL containing anything outside the legal URL character set is refused rather
  than escaped.
- **Offline means offline.** `--offline` never opens a connection.

What Hive does *not* do yet: signed packages, and any check on what a package's
code does once you import it. Installing a package runs no install scripts — but
importing one runs its code, so treat a package the way you'd treat any
dependency.
