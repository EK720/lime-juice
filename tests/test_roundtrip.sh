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
# semantic round-trip test for the compiler
# usage: ./test_roundtrip.sh <directory> [--strict]
# tests all .rkt files (excluding .mes.rkt) in the given directory.
# compiles each rkt to mes, decompiles back, and compares.

set -uo pipefail

DIR="$1"
STRICT="${2:-}"
PASS=0
FAIL=0
ERR=0

OUTDIR="$(pwd)/../test_output"
mkdir -p "$OUTDIR"

# clean previous results
rm -f "$OUTDIR"/*_compiled.mes "$OUTDIR"/*_compiled.rkt

while IFS= read -r -d '' rkt; do
    base=$(basename "$rkt" .rkt)
    compiled="$OUTDIR/${base}_compiled.mes"
    redecomp="$OUTDIR/${base}_compiled.rkt"

    # compile (engine/charset auto-detected from meta)
    if ! ../build/juice -c -f -o "$compiled" "$rkt" > /dev/null 2>"$OUTDIR/err.tmp"; then
        echo "COMPILE ERROR: $base: $(cat "$OUTDIR/err.tmp")"
        ERR=$((ERR+1))
        continue
    fi

    if [ ! -f "$compiled" ]; then
        echo "COMPILE ERROR: $base: output .mes not created"
        ERR=$((ERR+1))
        continue
    fi

    # detect engine and extraop from the rkt meta for decompile
    ENGINE="ADV"
    if grep -q "engine 'AI5" "$rkt"; then ENGINE="AI5"; fi
    if grep -q "engine 'AI1" "$rkt"; then ENGINE="AI1"; fi
    if grep -q "engine 'GM" "$rkt"; then ENGINE="GM"; fi

    EXTRA=""
    if grep -q "extraop #t" "$rkt"; then EXTRA="-E"; fi

    # decompile back
    if ! ../build/juice -d -f -e "$ENGINE" $EXTRA -o "$redecomp" "$compiled" > /dev/null 2>"$OUTDIR/err.tmp"; then
        warn=$(cat "$OUTDIR/err.tmp")
        if [ -n "$warn" ]; then
            echo "DECOMPILE WARNING: $base: $warn"
        fi
    fi

    if [ ! -f "$redecomp" ]; then
        echo "DECOMPILE ERROR: $base: output file not created"
        ERR=$((ERR+1))
        continue
    fi

    # compare (ignore whitespace differences for sound symbol trailing spaces)
    if [ "$STRICT" = "--strict" ]; then
        DIFF_ARGS=""
    else
        DIFF_ARGS="-b"
    fi

    if diff -q $DIFF_ARGS "$rkt" "$redecomp" > /dev/null 2>&1; then
        PASS=$((PASS+1))
    else
        echo "DIFF: $base"
        diff $DIFF_ARGS "$rkt" "$redecomp" 2>/dev/null | head -8 || true
        FAIL=$((FAIL+1))
    fi
done < <(find "$DIR" -name "*.rkt" ! -name "*.mes.rkt" -print0 | sort -z)

echo ""
echo "Results: $PASS passed, $FAIL failed, $ERR errors (total: $((PASS+FAIL+ERR)))"
