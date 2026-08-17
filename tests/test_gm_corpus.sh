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
# CORPUS may be one MES file or a directory. Both unpacked Fermion files and
# retail 0xff-packed Be-Yond files are accepted. The test checks auto-detection,
# lossless semantic round trips, deterministic packing, and changed-length
# relocation by appending one encodable character to every mode-1 text record.

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
packed_files=0
mode1_files=0
mode1_records=0

while IFS= read -r -d '' mes; do
    CURRENT="$mes"
    files=$((files + 1))

    "$JUICE" -d -f --auto-engine -o "$TMP/original.rkt" "$mes" >/dev/null
    grep -q "(engine 'GM)" "$TMP/original.rkt"
    ! grep -q '(raw ' "$TMP/original.rkt"

    "$JUICE" -c -f -o "$TMP/roundtrip.mes" "$TMP/original.rkt" >/dev/null
    first_byte="$(od -An -tu1 -N1 "$mes" | tr -d ' ')"
    if [ "$first_byte" = 255 ]; then
        packed_files=$((packed_files + 1))
        test "$(od -An -tu1 -N1 "$TMP/roundtrip.mes" | tr -d ' ')" = 255

        # The compressor is deterministic, but intentionally canonical rather
        # than an attempt to reproduce the retail token choices byte-for-byte.
        "$JUICE" -d -f --auto-engine -o "$TMP/canonical.rkt" \
            "$TMP/roundtrip.mes" >/dev/null
        cmp "$TMP/original.rkt" "$TMP/canonical.rkt"
        "$JUICE" -c -f -o "$TMP/canonical.mes" "$TMP/canonical.rkt" >/dev/null
        cmp "$TMP/roundtrip.mes" "$TMP/canonical.mes"
    else
        cmp "$mes" "$TMP/roundtrip.mes"
    fi

    text_count=$(grep -c '^[[:space:]]*(text ' "$TMP/original.rkt" || true)
    mode2_count=$(grep -c '^[[:space:]]*(text #:mode 2 ' "$TMP/original.rkt" || true)
    record_count=$((text_count - mode2_count))
    if [ "$record_count" -eq 0 ]; then
        continue
    fi

    mode1_files=$((mode1_files + 1))
    mode1_records=$((mode1_records + record_count))

    perl -pe 's/^(\s*\(text (?!#:mode 2\b).*)\)$/$1 "あ")/' \
        "$TMP/original.rkt" > "$TMP/grown.rkt"

    grown_count=$(grep -c '^[[:space:]]*(text .* "あ")$' "$TMP/grown.rkt" || true)
    if [ "$grown_count" -ne "$record_count" ] || cmp -s "$TMP/original.rkt" "$TMP/grown.rkt"; then
        echo "failed to grow every mode-1 record" >&2
        exit 1
    fi

    "$JUICE" -c -f -o "$TMP/grown.mes" "$TMP/grown.rkt" >/dev/null
    if [ "$first_byte" != 255 ] && \
       [ "$(wc -c < "$TMP/grown.mes")" -le "$(wc -c < "$mes")" ]; then
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

printf 'GM corpus: %d files (%d packed) round-tripped; %d mode-1 records grown across %d files\n' \
    "$files" "$packed_files" "$mode1_records" "$mode1_files"
