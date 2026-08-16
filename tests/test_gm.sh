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
# Synthetic General Message detection and byte-exact round-trip test.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
JUICE="${1:-$ROOT/build/juice}"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/lime-juice-gm.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

# Exercise the structural detector without the stronger SYSTEM.MLL startup
# signature. Mode-2 newlines must remain valid GM text tokens.
printf '%b' '\x02\x00\x4a\x02A\x04B\x00\x4a\x02C\x04D\x00\x00' > "$TMP/heuristic.mes"
"$JUICE" -d -f --auto-engine -o "$TMP/heuristic.rkt" "$TMP/heuristic.mes"
grep -q "(engine 'GM)" "$TMP/heuristic.rkt"
grep -q '(gm-text 2 "A\\nB")' "$TMP/heuristic.rkt"
grep -q '(gm-text 2 "C\\nD")' "$TMP/heuristic.rkt"
"$JUICE" -c -f -o "$TMP/heuristic-output.mes" "$TMP/heuristic.rkt"
cmp "$TMP/heuristic.mes" "$TMP/heuristic-output.mes"

# Header: two dictionary entries (あ, い). Code contains the observed GM
# SYSTEM.MLL loader signature, a call with a local continuation at 0x27 and an
# external MLL target at 0x7912, mode-1 dictionary/raw text, and mode-2 ASCII.
printf '%b' '\x06\x00\x82\xa0\x82\xa2\x6e\x11system.mll\x00\x00\x40\x27\x00\x12\x79\x00\x4a\x01\x18\x19\x04\x82\xa4\x00\x4a\x02BS\x00\x00' > "$TMP/input.mes"

"$JUICE" -d -f --auto-engine -o "$TMP/output.rkt" "$TMP/input.mes"
grep -q "(engine 'GM)" "$TMP/output.rkt"
grep -q '(gm-layout' "$TMP/output.rkt"
grep -q '(reloc 21 39)' "$TMP/output.rkt"
grep -q '(gm-text 1 "あい\\nう")' "$TMP/output.rkt"
grep -q '(gm-text 2 "BS")' "$TMP/output.rkt"

"$JUICE" -c -f -o "$TMP/output.mes" "$TMP/output.rkt"
cmp "$TMP/input.mes" "$TMP/output.mes"

# Grow mode-1 text through both dictionary and raw Shift-JIS paths. The local
# continuation moves by one byte while the external MLL target is unchanged.
sed 's/(gm-text 1 "あい\\nう")/(gm-text 1 "あい\\nうあ")/' \
    "$TMP/output.rkt" > "$TMP/mode1-edited.rkt"
"$JUICE" -c -f -o "$TMP/mode1-edited.mes" "$TMP/mode1-edited.rkt"
printf '%b' '\x06\x00\x82\xa0\x82\xa2\x6e\x11system.mll\x00\x00\x40\x28\x00\x12\x79\x00\x4a\x01\x18\x19\x04\x82\xa4\x18\x00\x4a\x02BS\x00\x00' > "$TMP/expected-mode1.mes"
cmp "$TMP/expected-mode1.mes" "$TMP/mode1-edited.mes"

# The game preset selects GM without relying on auto-detection.
"$JUICE" -d -f -p fermion -o "$TMP/preset.rkt" "$TMP/input.mes"
grep -q "(engine 'GM)" "$TMP/preset.rkt"

sed 's/(gm-text 2 "BS")/(gm-text 2 "BIGGER")/' \
    "$TMP/output.rkt" > "$TMP/edited.rkt"
"$JUICE" -c -f -o "$TMP/edited.mes" "$TMP/edited.rkt"

# Growing the second text record by four bytes moves the continuation from
# 0x27 to 0x2b. The external MLL call target remains exactly 0x7912.
printf '%b' '\x06\x00\x82\xa0\x82\xa2\x6e\x11system.mll\x00\x00\x40\x2b\x00\x12\x79\x00\x4a\x01\x18\x19\x04\x82\xa4\x00\x4a\x02BIGGER\x00\x00' > "$TMP/expected.mes"
cmp "$TMP/expected.mes" "$TMP/edited.mes"

sed 's/(gm-text 2 "BS")/(gm-text 2 "B\\nS")/' \
    "$TMP/output.rkt" > "$TMP/newline.rkt"
"$JUICE" -c -f -o "$TMP/newline.mes" "$TMP/newline.rkt"
printf '%b' '\x06\x00\x82\xa0\x82\xa2\x6e\x11system.mll\x00\x00\x40\x28\x00\x12\x79\x00\x4a\x01\x18\x19\x04\x82\xa4\x00\x4a\x02B\x04S\x00\x00' > "$TMP/expected-newline.mes"
cmp "$TMP/expected-newline.mes" "$TMP/newline.mes"
"$JUICE" -d -f --auto-engine -o "$TMP/newline-output.rkt" "$TMP/newline.mes"
grep -q '(gm-text 2 "B\\nS")' "$TMP/newline-output.rkt"

sed 's/(raw 110/(raw 0 110/' "$TMP/output.rkt" > "$TMP/bad-raw.rkt"
"$JUICE" -c -f -o "$TMP/bad-raw.mes" "$TMP/bad-raw.rkt" \
    > "$TMP/bad-raw.log" 2>&1 || true

if [ -e "$TMP/bad-raw.mes" ]; then
    echo "GM compiler accepted a length-changing raw edit" >&2
    exit 1
fi

grep -q 'raw nodes with layout metadata cannot change length' "$TMP/bad-raw.log"

# Hand-written GM sources have no layout metadata, but they still cannot exceed
# the engine's 16-bit file-address space.
awk 'BEGIN {
    print "(mes"
    print " (meta (engine '\''GM) (charset \"pc98\"))"
    print " (dict)"
    printf " (raw"
    for (i = 0; i < 65535; i++) printf " 0"
    print "))"
}' > "$TMP/oversize.rkt"

"$JUICE" -c -f -o "$TMP/oversize.mes" "$TMP/oversize.rkt" \
    > "$TMP/oversize.log" 2>&1 || true

if [ -e "$TMP/oversize.mes" ]; then
    echo "GM compiler left an oversized output file behind" >&2
    exit 1
fi

grep -q 'compiled file exceeds the 16-bit address space' "$TMP/oversize.log"

echo "GM synthetic round-trip and relocation: passed"
