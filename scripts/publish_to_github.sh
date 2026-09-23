#!/usr/bin/env bash
# One-shot: put this project on your GitHub and kick off the first release build.
#
#   ./scripts/publish_to_github.sh [repo-name]      (default: schwung-sculpt)
#
# Needs the GitHub CLI (https://cli.github.com, `brew install gh` on macOS).
# It will open a browser to sign you in the first time.
set -euo pipefail
cd "$(dirname "$0")/.."

REPO="${1:-schwung-sculpt}"
TAG="v$(sed -n 's/.*"version"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' src/module.json | head -1)"

if ! command -v gh >/dev/null 2>&1; then
    echo "GitHub CLI not found. Install it with:  brew install gh   (or see https://cli.github.com)"
    exit 1
fi
# the "workflow" scope is needed to push .github/workflows/release.yml
if gh auth status >/dev/null 2>&1; then gh auth refresh -h github.com -s workflow || true
else gh auth login --web --git-protocol https --scopes workflow; fi
gh auth setup-git >/dev/null 2>&1 || true

USER="$(gh api user --jq .login)"
echo "==> Publishing as $USER/$REPO ($TAG)"

# point release.json at your repo (the Schwung catalog reads it)
perl -pi -e "s#YOUR_GITHUB_USER/schwung-sculpt#$USER/$REPO#g" release.json

if [ ! -d .git ]; then
    git init -q -b main
fi
git add -A
git -c user.name="${GIT_AUTHOR_NAME:-$USER}" -c user.email="${GIT_AUTHOR_EMAIL:-$USER@users.noreply.github.com}" \
    commit -q -m "Sculpt $TAG: four-track sculpting sampler for Ableton Move (Schwung)" || true

if gh repo view "$USER/$REPO" >/dev/null 2>&1; then
    git remote get-url origin >/dev/null 2>&1 || git remote add origin "https://github.com/$USER/$REPO.git"
    git push -u origin main
else
    gh repo create "$REPO" --public --source=. --remote=origin --push \
        --description "Sculpt: a four-track sculpting sampler (48-band resonant filter bank, granular, scenes) for Ableton Move via Schwung"
fi

# tagging triggers .github/workflows/release.yml: tests, ARM64 build, release with sculpt-module.tar.gz
if ! git rev-parse "$TAG" >/dev/null 2>&1; then git tag "$TAG"; fi
git push origin "$TAG"

echo ""
echo "Done."
echo "  Repo:     https://github.com/$USER/$REPO"
echo "  Build:    https://github.com/$USER/$REPO/actions   (takes ~3 minutes)"
echo "  Release:  https://github.com/$USER/$REPO/releases/tag/$TAG   (sculpt-module.tar.gz appears when the build finishes)"
