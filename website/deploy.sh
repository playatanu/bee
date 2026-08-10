#!/usr/bin/env bash
#
# Publish the Bee documentation to GitHub Pages.
#
# It builds the site and force-pushes the result to the `gh-pages` branch of
# `origin` (https://github.com/beelang-project/bee). Your normal source on
# `main` is untouched - only the generated `gh-pages` branch is updated.
#
# Usage:
#   ./deploy.sh
#
# One-time GitHub setting (after the first run creates the branch):
#   Settings -> Pages -> Build and deployment -> Source: "Deploy from a branch"
#   Branch: gh-pages   Folder: / (root)
#
# The site then goes live at:  https://beelang-project.github.io/bee/

set -euo pipefail

# Run from this script's directory (the website/ folder), wherever it's called from.
cd "$(dirname "$0")"

PY="${PYTHON:-python3}"

# Make sure the Material theme is available.
if ! "$PY" -c "import material" >/dev/null 2>&1; then
  echo "Installing docs dependencies..."
  "$PY" -m pip install -r docs-requirements.txt
fi

# Build (strict = fail on any broken link) and push to gh-pages.
"$PY" -m mkdocs gh-deploy \
  --strict \
  --clean \
  --remote-name origin \
  --remote-branch gh-pages \
  --message "Deploy docs {sha} (mkdocs {version})"

echo
echo "Done. If Pages is configured for the gh-pages branch, your docs are live at:"
echo "  https://beelang-project.github.io/bee/"
