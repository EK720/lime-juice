#!/bin/bash
#
# lime-juice: C++ port of Tomyun's "Juice" de/recompiler for PC-98 games
# Copyright (C) 2026 Fuzion
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
#
# Regression tests for GM corpus-runner failure detection; no game data needed.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
JUICE="${1:-$ROOT/build/juice}"
JUICE="$(cd "$(dirname "$JUICE")" && pwd)/$(basename "$JUICE")"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/lime-juice-gm-harness.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

cat > "$TMP/scene.rkt" <<'EOF'
(mes (meta (engine 'GM) (charset "pc98")) (dict)
 (text "あ") (text "い") (end))
EOF
sed "s/(engine 'GM)/(engine 'GM) (compression 'beyond)/" \
    "$TMP/scene.rkt" > "$TMP/packed.rkt"
for kind in scene packed; do
    mkdir "$TMP/$kind"
    "$JUICE" -c -f -o "$TMP/$kind/a.mes" "$TMP/$kind.rkt" > "$TMP/build.log" 2>&1
    test -s "$TMP/$kind/a.mes"
    cp "$TMP/$kind/a.mes" "$TMP/$kind/b.mes"
    bash "$ROOT/tests/test_gm_corpus.sh" "$TMP/$kind" "$JUICE" > "$TMP/baseline.log" 2>&1
    grep -q 'GM corpus: 2 files' "$TMP/baseline.log"
done

cat > "$TMP/juice-fault" <<'EOF'
#!/bin/bash
set -eu
output=""
original_args=("$@")
while [ "$#" -gt 0 ]; do
    if [ "$1" = -o ]; then output="$2"; break; fi
    shift
done
if [[ "$output" == */"$GM_FAULT_STAGE" ]]; then
    if [ -e "$GM_FAULT_SEEN" ]; then
        case "$GM_FAULT_MODE" in
            missing) exit 0 ;;
            nonzero) exit 1 ;;
            diagnostic)
                "$GM_REAL_JUICE" "${original_args[@]}"
                echo 'injected error after output' >&2
                exit 0 ;;
        esac
    fi
    touch "$GM_FAULT_SEEN"
fi
exec "$GM_REAL_JUICE" "${original_args[@]}"
EOF
chmod +x "$TMP/juice-fault"
export GM_REAL_JUICE="$JUICE" GM_FAULT_SEEN="$TMP/seen"
for kind in scene packed; do
    for GM_FAULT_STAGE in grown.mes grown-output.rkt; do
        for GM_FAULT_MODE in missing nonzero diagnostic; do
            export GM_FAULT_STAGE GM_FAULT_MODE
            rm -f "$GM_FAULT_SEEN"
            if bash "$ROOT/tests/test_gm_corpus.sh" "$TMP/$kind" "$TMP/juice-fault" \
                > "$TMP/fault.log" 2>&1; then
                echo "corpus runner accepted $kind $GM_FAULT_STAGE $GM_FAULT_MODE failure" >&2
                exit 1
            fi
            grep -q 'GM corpus failure:' "$TMP/fault.log"
            ! grep -q 'GM corpus: 2 files' "$TMP/fault.log"
        done
    done
done

echo 'GM corpus harness: passed (12 injected failures rejected)'
