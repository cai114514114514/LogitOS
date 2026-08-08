#!/bin/bash
# build_apps.sh -- scaffold, build and pack the framework corpus.
#
#     bash tests/fixtures/frameworks/build_apps.sh [workdir]
#
# NOT run at test time. `make test-frameworks` reads the committed bytes under
# tests/fixtures/frameworks/<name>/ and touches no network -- same rule as
# tests/fixtures/webapi and tests/fixtures/cssweb, and for the same reason: a
# corpus fetched or rebuilt at measure time changes under the measurement, and
# then "the exception count moved" says nothing about this browser. This script
# is how the committed bytes were PRODUCED, kept so they can be reproduced or
# refreshed deliberately.
#
# WHAT IT BUILDS, AND WHY EACH ONE IS A REAL APP
# Every fixture is a minimal but real application built by the framework's OWN
# default toolchain -- the `create` template, untouched except for the app
# source -- with:
#   * a component holding state          (React useState / Vue ref / Svelte
#                                         $state / Angular signal)
#   * a click handler that mutates it
#   * a LAZILY IMPORTED route            so the bundler's chunk-loading runtime
#                                         is actually emitted and actually runs
#
# The lazy route renders AT STARTUP rather than behind the click, which is the
# one deliberate departure from a normal app and is load-bearing: the probe
# injects no input, so a chunk loader reachable only by a click is a chunk
# loader this corpus would never measure. The click handler is still there and
# still wired; it is simply not what triggers the split.
#
# A hand-written <script> pulling React off a CDN would exercise NONE of this --
# no bundler runtime, no publicPath computation, no code splitting -- and the
# whole corpus would have measured nothing. That is why the toolchains are real.
#
# WHY node RUNS ON THE WINDOWS SIDE
# This repository builds only inside WSL, and that rule is about the C
# toolchain. There is no node in this WSL image; npm here is the Windows
# install on the PATH. Building a JavaScript bundle is not building LogitOS, so
# it runs where node is, and only the produced bytes come back into the tree.
#
# WEBPACK HAS NO `create` TEMPLATE, so its project is the four files webpack's
# own documentation calls a minimal setup -- entry, output, html-webpack-plugin,
# and one dynamic import. The configuration is deliberately not tuned: what is
# being measured is the runtime webpack EMITS, not a configuration of ours.
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
WORK="${1:-${TMPDIR:-/tmp}/logitos-fwcorpus}"
mkdir -p "$WORK"

if ! command -v npm >/dev/null 2>&1; then
    echo "build_apps: no npm on PATH -- the corpus cannot be rebuilt here."
    echo "  The committed fixtures under $HERE are what the numbers were taken on."
    exit 2
fi

SRC="$HERE/_src"

# ---- scaffold ------------------------------------------------------------
# Non-interactive flags throughout: a prompt in a build script is a build
# script that hangs in CI and passes by never finishing.
scaffold() {
    local name="$1"; shift
    [ -d "$WORK/$name" ] && return 0
    ( cd "$WORK" && "$@" </dev/null ) || return 1
}

echo "== scaffolding into $WORK =="
scaffold react   npm create vite@latest react   -- --template react-ts  --yes
scaffold vue     npm create vite@latest vue     -- --template vue-ts    --yes
scaffold svelte  npm create vite@latest svelte  -- --template svelte-ts --yes
scaffold vite    npm create vite@latest vite    -- --template vanilla-ts --yes
scaffold next    npx --yes create-next-app@latest next --ts --app --no-tailwind \
                     --no-eslint --no-src-dir --no-turbopack --import-alias "@/*" \
                     --use-npm --skip-install
scaffold angular npx --yes @angular/cli@latest new angular --directory=angular \
                     --routing --style=css --ssr=false --skip-git --skip-install --defaults
mkdir -p "$WORK/webpack"

# ---- overlay the committed app sources -----------------------------------
# _src/<name>/ mirrors the paths inside the scaffold. Copying over the template
# is what keeps the app sources in git without committing seven node_modules.
echo "== overlaying $SRC =="
for d in "$SRC"/*/; do
    n="$(basename "$d")"
    [ -d "$WORK/$n" ] || { echo "  (no scaffold for $n)"; continue; }
    ( cd "$d" && find . -type f -print0 | while IFS= read -r -d '' f; do
        mkdir -p "$WORK/$n/$(dirname "$f")"
        cp "$f" "$WORK/$n/$f"
      done )
    # Angular's generated spec file references the component API we replaced.
    rm -f "$WORK/$n/src/app/app.spec.ts"
done

# ---- install + build ------------------------------------------------------
for n in react vue svelte vite webpack next angular; do
    echo "== $n =="
    ( cd "$WORK/$n" && npm install --no-audit --no-fund >/dev/null 2>&1 \
                    && npm run build >/dev/null 2>&1 ) \
        || { echo "  BUILD FAILED -- rerun by hand in $WORK/$n"; continue; }
done

# ---- pack -----------------------------------------------------------------
# Angular writes to dist/<project>/browser; everything else to dist/.
echo "== packing =="
pack() { python3 "$HERE/pack.py" "$1" "$2"; }
pack react   "$WORK/react/dist"
pack vue     "$WORK/vue/dist"
pack svelte  "$WORK/svelte/dist"
pack vite    "$WORK/vite/dist"
pack webpack "$WORK/webpack/dist"
pack next    "$WORK/next/out"
pack angular "$WORK/angular/dist/angular/browser"

echo
echo "packed into $HERE -- now: make probe-frameworks"
