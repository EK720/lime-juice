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
# Usage: tests/test_gm_corpus.sh CORPUS [JUICE]
#
# CORPUS may be one MES file or a directory. The test checks GM auto-detection,
# byte-exact no-op round trips, and changed-length relocation by appending one
# encodable character to every mode-1 text record. No corpus data is copied into
# the repository.

set -euo pipefail

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    echo "usage: $0 CORPUS [JUICE]" >&2
    exit 2
fi

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CORPUS="$1"
JUICE="${2:-$ROOT/build/juice}"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/lime-juice-gm-corpus.XXXXXX")"
CURRENT=""
trap 'rm -rf "$TMP"' EXIT
trap 'echo "GM corpus failure: $CURRENT" >&2' ERR

if [ ! -x "$JUICE" ]; then
    echo "juice binary is not executable: $JUICE" >&2
    exit 2
fi

mes_files() {
    if [ -f "$CORPUS" ]; then
        printf '%s\0' "$CORPUS"
    elif [ -d "$CORPUS" ]; then
        find "$CORPUS" -type f -iname '*.mes' -print0
    else
        echo "corpus path does not exist: $CORPUS" >&2
        return 2
    fi
}

files=0
mode1_files=0
mode1_records=0

while IFS= read -r -d '' mes; do
    CURRENT="$mes"
    files=$((files + 1))

    "$JUICE" -d -f --auto-engine -o "$TMP/original.rkt" "$mes" >/dev/null
    grep -q "(engine 'GM)" "$TMP/original.rkt"

    "$JUICE" -c -f -o "$TMP/roundtrip.mes" "$TMP/original.rkt" >/dev/null
    cmp "$mes" "$TMP/roundtrip.mes"

    record_count=$(grep -c '^[[:space:]]*(gm-text 1 ' "$TMP/original.rkt" || true)
    if [ "$record_count" -eq 0 ]; then
        continue
    fi

    mode1_files=$((mode1_files + 1))
    mode1_records=$((mode1_records + record_count))

    sed -E 's/^([[:space:]]*\(gm-text 1 ".*)"\)$/\1あ"\)/' \
        "$TMP/original.rkt" > "$TMP/grown.rkt"

    grown_count=$(grep -c '^[[:space:]]*(gm-text 1 ' "$TMP/grown.rkt" || true)
    if [ "$grown_count" -ne "$record_count" ] || cmp -s "$TMP/original.rkt" "$TMP/grown.rkt"; then
        echo "failed to grow every mode-1 record" >&2
        exit 1
    fi

    "$JUICE" -c -f -o "$TMP/grown.mes" "$TMP/grown.rkt" >/dev/null
    if [ "$(wc -c < "$TMP/grown.mes")" -le "$(wc -c < "$mes")" ]; then
        echo "changed-length compile did not grow the MES file" >&2
        exit 1
    fi

    # Decompilation runs the complete structural walker and rejects stale local
    # targets, so this validates every relocated output instruction boundary.
    "$JUICE" -d -f -e GM -o "$TMP/grown-output.rkt" "$TMP/grown.mes" >/dev/null
    grep -q "(engine 'GM)" "$TMP/grown-output.rkt"
done < <(mes_files)

if [ "$files" -eq 0 ]; then
    echo "no MES files found in corpus: $CORPUS" >&2
    exit 1
fi

printf 'GM corpus: %d files round-tripped; %d mode-1 records grown across %d files\n' \
    "$files" "$mode1_records" "$mode1_files"
