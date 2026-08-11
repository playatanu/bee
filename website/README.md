# Bee documentation site

The Bee documentation, built with [MkDocs](https://www.mkdocs.org/) and the
[Material](https://squidfunk.github.io/mkdocs-material/) theme. It covers the
**language**, the **Hive** package manager (and C++ bindings), and the **VS Code
extension**.

## Preview locally

```bash
pip install -r docs-requirements.txt
mkdocs serve            # live-reloading preview at http://127.0.0.1:8000
```

## Build

```bash
mkdocs build --strict   # output in ./site (also what CI runs)
```

## Deploy to GitHub Pages

Both methods publish to the `gh-pages` branch of `origin`; the site then serves
at <https://playatanu.github.io/bee/>.

**Manually, right now:**

```bash
./deploy.sh          # builds and force-pushes to the gh-pages branch
```

**Automatically on every push to `main`:**
[`.github/workflows/docs.yml`](../.github/workflows/docs.yml) runs the same
`mkdocs gh-deploy` in CI.

**One-time GitHub setting** (after the first deploy creates the branch):
**Settings → Pages → Source: "Deploy from a branch" → Branch: `gh-pages` /(root)**.

## Structure

```
website/
├── mkdocs.yml               # site config, theme, navigation
├── docs-requirements.txt    # build dependency (mkdocs-material)
└── docs/
    ├── index.md             # landing page
    ├── getting-started.md   # install + first program
    ├── language.md          # full language reference
    ├── hive.md              # package manager
    ├── bindings.md          # C++ native modules / beegen
    ├── editor.md            # VS Code extension
    ├── performance.md       # JIT & performance notes
    ├── changelog.md         # release history
    ├── assets/              # logo / favicon
    └── stylesheets/         # honey + green brand theme
```

The reference pages (`language.md`, `hive.md`, `bindings.md`, `performance.md`)
mirror the source docs in [`../docs/`](../docs/). When you edit one, update the
other so the repo and the site stay in sync.
