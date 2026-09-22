#!/bin/bash
# Run cppcheck and clang-tidy over the package's translation units.
# Usage: static_analysis.sh <compile_commands.json>
# Build the package with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON to get the compile database.
# CLANG_TIDY_ARGS holds extra clang-tidy options, e.g. "--extra-arg-before=-isystem/usr/include/c++/11".
set -u
DB=${1:?usage: static_analysis.sh <compile_commands.json>}
PKG="$(cd "$(dirname "$0")/.." && pwd)"
status=0

# Only this package's own translation units are analysed, not vendored test frameworks.
own_db=$(mktemp --suffix=.json)
trap 'rm -f "$own_db"' EXIT
python3 -c "import json,sys; json.dump([e for e in json.load(open(sys.argv[1])) if e['file'].startswith(sys.argv[2] + '/')], open(sys.argv[3], 'w'))" "$DB" "$PKG" "$own_db"

if command -v cppcheck >/dev/null; then
    echo "== cppcheck =="
    cppcheck --project="$own_db" --enable=warning,style,performance,portability --inconclusive --std=c++17 \
        --suppress=missingIncludeSystem --suppressions-list="$PKG/cppcheck_suppressions.txt" --inline-suppr \
        --template='{file}:{line}: [{severity}:{id}] {message}' --error-exitcode=1 --quiet -j"$(nproc)" || status=1
else
    echo "cppcheck is not installed" >&2
    status=1
fi

if command -v clang-tidy >/dev/null; then
    echo "== clang-tidy =="
    files=$(python3 -c "import json,sys; print('\n'.join(sorted({e['file'] for e in json.load(open(sys.argv[1])) if e['file'].startswith(sys.argv[2] + '/src/')})))" "$DB" "$PKG")
    # shellcheck disable=SC2086
    clang-tidy -p "$(dirname "$DB")" --config-file="$PKG/.clang-tidy" --quiet ${CLANG_TIDY_ARGS:-} $files 2>/dev/null | grep -E "warning:|error:" && status=1
else
    echo "clang-tidy is not installed" >&2
fi
exit $status
