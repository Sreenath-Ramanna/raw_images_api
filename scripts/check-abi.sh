#!/usr/bin/env bash
# scripts/check-abi.sh — every function the headers promise is actually exported.
#
#   scripts/check-abi.sh [path/to/libraw_images_api.so]
#
# Two ways to ship a library that compiles, links and then fails at runtime:
#
#   1. A new public function without the RIA_API macro. Visibility is hidden
#      project-wide, so the symbol is simply absent from the .so and the
#      Flutter app dies on a lookup that has no build-time counterpart.
#   2. A change to the legacy raw_* ABI. raw_viewer links against it and is not
#      in this working tree, so nothing here notices.
#
# This compares both headers against the built object, which is the only place
# the answer actually lives.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIB="${1:-$ROOT/build/libraw_images_api.so}"

if [[ ! -f "$LIB" ]]; then
    echo "No library at $LIB — build first:" >&2
    echo "  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build" >&2
    exit 1
fi

# Declarations, one per line, comments gone. Declarations wrap across lines in
# the header, so this joins the file and re-splits it on the statement.
declarations() {
    # Preprocessor lines go first: the RIA_API macro definition itself is a
    # visibility attribute followed by a '(', and would otherwise read as a
    # declaration of a function called __attribute__.
    sed -e 's://.*::' "$1" \
        | sed -e ':a' -e 'N' -e '$!ba' -e 's:/\*[^*]*\*\+\([^/*][^*]*\*\+\)*/::g' \
        | grep -v '^[[:space:]]*#' \
        | tr '\n' ' ' | tr ';' '\n'
}

# The identifier immediately before the first '(' of a declaration is the
# function name; anything later is a function-pointer parameter.
names_from() {   # $1 = header, $2 = grep filter selecting exported decls
    declarations "$1" | grep -E "$2" \
        | grep -oE '[A-Za-z_][A-Za-z0-9_]*[[:space:]]*\(' \
        | sed -e 's/[[:space:]]*($//' -e 's/($//' \
        | awk '!seen[$0]++'
}

exported="$(nm -D --defined-only "$LIB" | awk '{print $NF}' | sort -u)"

missing=0
check() {   # $1 = human name, $2 = list of symbols
    local label="$1" count=0
    while read -r sym; do
        [[ -n "$sym" ]] || continue
        count=$((count + 1))
        if ! grep -qx -- "$sym" <<<"$exported"; then
            echo "  MISSING  $sym" >&2
            missing=$((missing + 1))
        fi
    done <<<"$2"
    echo "$label: $count declared"
}

# The public header declares one function per RIA_API. Take the first name on
# each such line only — hence head -1 per declaration.
public="$(declarations "$ROOT/include/raw_images_api.h" | grep 'RIA_API' \
    | grep -oE '[A-Za-z_][A-Za-z0-9_]*[[:space:]]*\(' | sed 's/[[:space:]]*(//' \
    | awk '!seen[$0]++')"
check "public RIA_API surface" "$public"

if [[ -f "$ROOT/include/raw_images_api_legacy.h" ]]; then
    legacy="$(declarations "$ROOT/include/raw_images_api_legacy.h" \
        | grep -oE '\braw_[a-z0-9_]+[[:space:]]*\(' | sed 's/[[:space:]]*(//' \
        | awk '!seen[$0]++')"
    check "legacy raw_* ABI" "$legacy"
fi

if (( missing )); then
    echo >&2
    echo "$missing declared symbol(s) are not exported by $LIB." >&2
    echo "A public function needs the RIA_API macro; the legacy ABI needs" >&2
    echo "src/ria_legacy.c to stay compiled with -fvisibility=default." >&2
    exit 1
fi

echo "All declared symbols are exported by $(basename "$LIB")."
