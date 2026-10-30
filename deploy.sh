#!/usr/bin/env bash
# ====================================================================
#  deploy.sh  --  build, commit, push, and publish The Housekeeper
#
#  First run:   creates the GitHub repo, pushes, enables GitHub Pages
#               serving docs/ from main.
#  Later runs:  rebuilds WASM, commits whatever changed, pushes.
#
#  Usage:  ./deploy.sh ["commit message"]
#
#  Needs:  git, gh (logged in: gh auth login), emcc on PATH
#          (source ~/emsdk/emsdk_env.sh if `emcc` is not found)
#
#  Override the repo name / visibility with environment variables:
#          REPO=housekeeper VISIBILITY=private ./deploy.sh
# ====================================================================
set -euo pipefail

REPO="${REPO:-carpet_defense}"
VISIBILITY="${VISIBILITY:-public}"          # public | private
BRANCH="${BRANCH:-main}"
MSG="${1:-Update $(date +%Y-%m-%d)}"

cd "$(dirname "$0")"

say()  { printf '\033[1;35m>> %s\033[0m\n' "$*"; }
die()  { printf '\033[1;31m!! %s\033[0m\n' "$*" >&2; exit 1; }

# -- prerequisites ----------------------------------------------------
command -v git >/dev/null || die "git not found"
command -v gh  >/dev/null || die "gh not found  (https://cli.github.com)"
gh auth status >/dev/null 2>&1 || die "gh is not logged in: run  gh auth login"

if ! command -v emcc >/dev/null; then
    # try the usual emsdk location before giving up
    if [ -f "$HOME/emsdk/emsdk_env.sh" ]; then
        # shellcheck disable=SC1091
        source "$HOME/emsdk/emsdk_env.sh" >/dev/null 2>&1
    fi
    command -v emcc >/dev/null || die "emcc not found: source ~/emsdk/emsdk_env.sh"
fi

OWNER="$(gh api user -q .login)"

# -- build --------------------------------------------------------------
say "native build (sanity check)"
make -s

say "WASM build -> docs/"
make -s wasm
[ -f docs/index.html ] && [ -f docs/index.wasm ] || die "WASM build produced no output"

# -- git ----------------------------------------------------------------
if [ ! -d .git ]; then
    say "git init"
    git init -q -b "$BRANCH"
fi

# make sure we are on the deploy branch
if git rev-parse --verify -q HEAD >/dev/null; then
    # repo has commits: switch (or create) normally
    git rev-parse --verify -q "$BRANCH" >/dev/null && git checkout -q "$BRANCH" \
                                                   || git checkout -q -b "$BRANCH"
else
    # unborn repo: just point HEAD at the branch; first commit creates it
    git symbolic-ref HEAD "refs/heads/$BRANCH"
fi

git add -A
if git diff --cached --quiet; then
    say "nothing new to commit"
else
    git commit -q -m "$MSG"
    say "committed: $MSG"
fi

# -- remote / repo ------------------------------------------------------
if ! git remote get-url origin >/dev/null 2>&1; then
    if gh repo view "$OWNER/$REPO" >/dev/null 2>&1; then
        say "repo $OWNER/$REPO exists; adding remote"
        git remote add origin "https://github.com/$OWNER/$REPO.git"
    else
        say "creating GitHub repo $OWNER/$REPO ($VISIBILITY)"
        gh repo create "$REPO" "--$VISIBILITY" \
            --description "The Housekeeper: a Space Invaders variant in C17/SDL2, native + WebAssembly" \
            --source=. --remote=origin
    fi
fi

say "pushing $BRANCH"
git push -u origin "$BRANCH" -q

# -- GitHub Pages ---------------------------------------------------------
PAGES_URL="https://$OWNER.github.io/$REPO/"

if gh api "repos/$OWNER/$REPO/pages" >/dev/null 2>&1; then
    # already enabled: make sure it points at main:/docs
    gh api -X PUT "repos/$OWNER/$REPO/pages" \
        -f "source[branch]=$BRANCH" -f "source[path]=/docs" >/dev/null 2>&1 || true
    say "GitHub Pages already enabled"
else
    say "enabling GitHub Pages from $BRANCH:/docs"
    gh api -X POST "repos/$OWNER/$REPO/pages" \
        -f "build_type=legacy" \
        -f "source[branch]=$BRANCH" -f "source[path]=/docs" >/dev/null
fi

# add the live URL to the repo sidebar (harmless if it fails)
gh repo edit "$OWNER/$REPO" --homepage "$PAGES_URL" >/dev/null 2>&1 || true

say "done"
echo
echo "   repo:  https://github.com/$OWNER/$REPO"
echo "   game:  $PAGES_URL      (first deploy takes a minute or two)"
echo
