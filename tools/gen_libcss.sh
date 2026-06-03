#!/bin/sh
# Regenerate LibCSS's code-generation outputs, which are committed (vendored)
# under third_party/css so the normal build needs no perl/host-compiler step.
# Run from the repo root. Needs host clang + perl. Idempotent.
set -e
CSS=third_party/css/libcss
PU=third_party/css/libparserutils

# 1) Simple CSS property parsers: one .c per non-comment line of properties.gen.
#    (Complex properties -- background/border/font/... -- are hand-written .c.)
clang -O2 -w -o /tmp/propgen "$CSS/src/parse/properties/css_property_parser_gen.c"
mkdir -p "$CSS/src/parse/properties/autogen"
n=0
while IFS= read -r line; do
  case "$line" in ''|\#*) continue;; esac
  key="${line%%:*}"
  /tmp/propgen -o "$CSS/src/parse/properties/autogen/${key}.c" "$line"
  n=$((n+1))
done < "$CSS/src/parse/properties/properties.gen"
echo "generated $n property parsers -> $CSS/src/parse/properties/autogen/"

# 2) Charset alias table (libparserutils): build/Aliases -> src/charset/aliases.inc
( cd "$PU" && perl build/make-aliases.pl )
echo "generated $PU/src/charset/aliases.inc"
