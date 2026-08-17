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
grep -q '(text #:mode 2 "A\\nB")' "$TMP/heuristic.rkt"
grep -q '(text #:mode 2 "C\\nD")' "$TMP/heuristic.rkt"
"$JUICE" -c -f -o "$TMP/heuristic-output.mes" "$TMP/heuristic.rkt"
cmp "$TMP/heuristic.mes" "$TMP/heuristic-output.mes"

# Growing a canonical integer from one byte to two changes the size of the
# control instruction that owns its address field. The compiler chooses the
# width and moves the following instruction boundary from 0x08 to 0x09.
printf '%b' '\x02\x00\x33\x08\x00\x01\x01\x00\x00' > "$TMP/expression.mes"
"$JUICE" -d -f -e GM -o "$TMP/expression.rkt" "$TMP/expression.mes"
grep -q '(if (local-address 8) 1)' "$TMP/expression.rkt"
grep -q '(label 8)' "$TMP/expression.rkt"
sed 's/(if (local-address 8) 1)/(if (local-address 8) 256)/' \
    "$TMP/expression.rkt" > "$TMP/expression-edited.rkt"
"$JUICE" -c -f -o "$TMP/expression-edited.mes" "$TMP/expression-edited.rkt"
printf '%b' '\x02\x00\x33\x09\x00\x02\x00\x01\x00\x00' \
    > "$TMP/expected-expression.mes"
cmp "$TMP/expected-expression.mes" "$TMP/expression-edited.mes"

# Balanced postfix streams become the conventional nested Juice expression
# tree. Anomalous shipped streams still retain their native flat form.
printf '%b' '\x02\x00\x3b\x01\x02\x01\x03\x23\x00\x00' > "$TMP/balanced.mes"
"$JUICE" -d -f -e GM -o "$TMP/balanced.rkt" "$TMP/balanced.mes"
grep -q '(eval (+ 2 3))' \
    "$TMP/balanced.rkt"
"$JUICE" -c -f -o "$TMP/balanced-output.mes" "$TMP/balanced.rkt"
cmp "$TMP/balanced.mes" "$TMP/balanced-output.mes"

printf '%b' '\x02\x00\x3b\x01\x0a\x28\x00\x00' > "$TMP/trailing-operator.mes"
"$JUICE" -d -f -e GM -o "$TMP/trailing-operator.rkt" \
    "$TMP/trailing-operator.mes"
grep -q '(eval (postfix 10 ==))' \
    "$TMP/trailing-operator.rkt"
"$JUICE" -c -f -o "$TMP/trailing-operator-output.mes" \
    "$TMP/trailing-operator.rkt"
cmp "$TMP/trailing-operator.mes" "$TMP/trailing-operator-output.mes"

# Header: two dictionary entries (あ, い). Code contains the observed GM
# SYSTEM.MLL loader signature, a call with a local continuation at 0x27 and an
# external MLL target at 0x7912, mode-1 dictionary/raw text, and mode-2 ASCII.
printf '%b' '\x06\x00\x82\xa0\x82\xa2\x6e\x11system.mll\x00\x00\x40\x27\x00\x12\x79\x00\x4a\x01\x18\x19\x04\x82\xa4\x00\x4a\x02BS\x00\x00' > "$TMP/input.mes"

"$JUICE" -d -f --auto-engine -o "$TMP/output.rkt" "$TMP/input.mes"
grep -q "(engine 'GM)" "$TMP/output.rkt"
grep -q '(local-address 39)' "$TMP/output.rkt"
grep -q '(label 39)' "$TMP/output.rkt"
grep -q '(mll-load "system.mll")' "$TMP/output.rkt"
grep -q '(text "あい\\nう"' "$TMP/output.rkt"
grep -q '(text #:mode 2 "BS")' "$TMP/output.rkt"
! grep -q '(raw ' "$TMP/output.rkt"
! grep -q '(gm-' "$TMP/output.rkt"

"$JUICE" -c -f -o "$TMP/output.mes" "$TMP/output.rkt"
cmp "$TMP/input.mes" "$TMP/output.mes"

# Labels make structural edits relocatable too: insert a new node before the
# continuation without maintaining a parallel span table.
awk '/\(label 39\)/ { print " (text #:mode 2 \"X\")" } { print }' \
    "$TMP/output.rkt" > "$TMP/inserted.rkt"
"$JUICE" -c -f -o "$TMP/inserted.mes" "$TMP/inserted.rkt"
printf '%b' '\x06\x00\x82\xa0\x82\xa2\x6e\x11system.mll\x00\x00\x40\x2b\x00\x12\x79\x00\x4a\x01\x18\x19\x04\x82\xa4\x00\x4a\x02BS\x00\x4a\x02X\x00\x00' > "$TMP/expected-inserted.mes"
cmp "$TMP/expected-inserted.mes" "$TMP/inserted.mes"

grep -v '(label 39)' "$TMP/output.rkt" > "$TMP/missing-label.rkt"
"$JUICE" -c -f -o "$TMP/missing-label.mes" "$TMP/missing-label.rkt" \
    > "$TMP/missing-label.log" 2>&1 || true
test ! -e "$TMP/missing-label.mes"
grep -q 'local address has no matching label: 39' "$TMP/missing-label.log"

# Grow mode-1 text through both dictionary and raw Shift-JIS paths. The local
# continuation moves by one byte while the external MLL target is unchanged.
sed 's/(text "あい\\nう"/(text "あい\\nうあ"/' \
    "$TMP/output.rkt" > "$TMP/mode1-edited.rkt"
"$JUICE" -c -f -o "$TMP/mode1-edited.mes" "$TMP/mode1-edited.rkt"
printf '%b' '\x06\x00\x82\xa0\x82\xa2\x6e\x11system.mll\x00\x00\x40\x28\x00\x12\x79\x00\x4a\x01\x18\x19\x04\x82\xa4\x18\x00\x4a\x02BS\x00\x00' > "$TMP/expected-mode1.mes"
cmp "$TMP/expected-mode1.mes" "$TMP/mode1-edited.mes"

# The game preset selects GM without relying on auto-detection.
"$JUICE" -d -f -p fermion -o "$TMP/preset.rkt" "$TMP/input.mes"
grep -q "(engine 'GM)" "$TMP/preset.rkt"
"$JUICE" -d -f -p beyond -o "$TMP/beyond-preset.rkt" "$TMP/input.mes"
grep -q "(engine 'GM)" "$TMP/beyond-preset.rkt"
"$JUICE" -c -f -p beyond -o "$TMP/beyond-preset.mes" "$TMP/output.rkt"
test "$(od -An -tu1 -N1 "$TMP/beyond-preset.mes" | tr -d ' ')" = 255

sed 's/(text #:mode 2 "BS")/(text #:mode 2 "BIGGER")/' \
    "$TMP/output.rkt" > "$TMP/edited.rkt"
"$JUICE" -c -f -o "$TMP/edited.mes" "$TMP/edited.rkt"

# Growing the second text record by four bytes moves the continuation from
# 0x27 to 0x2b. The external MLL call target remains exactly 0x7912.
printf '%b' '\x06\x00\x82\xa0\x82\xa2\x6e\x11system.mll\x00\x00\x40\x2b\x00\x12\x79\x00\x4a\x01\x18\x19\x04\x82\xa4\x00\x4a\x02BIGGER\x00\x00' > "$TMP/expected.mes"
cmp "$TMP/expected.mes" "$TMP/edited.mes"

sed 's/(text #:mode 2 "BS")/(text #:mode 2 "B\\nS")/' \
    "$TMP/output.rkt" > "$TMP/newline.rkt"
"$JUICE" -c -f -o "$TMP/newline.mes" "$TMP/newline.rkt"
printf '%b' '\x06\x00\x82\xa0\x82\xa2\x6e\x11system.mll\x00\x00\x40\x28\x00\x12\x79\x00\x4a\x01\x18\x19\x04\x82\xa4\x00\x4a\x02B\x04S\x00\x00' > "$TMP/expected-newline.mes"
cmp "$TMP/expected-newline.mes" "$TMP/newline.mes"
"$JUICE" -d -f --auto-engine -o "$TMP/newline-output.rkt" "$TMP/newline.mes"
grep -q '(text #:mode 2 "B\\nS")' "$TMP/newline-output.rkt"

# Decompile controls affect only their intended layer. Commands remain
# semantic with --no-decode, while --no-resolve uses the cmd:N escape hatch.
"$JUICE" -d -f --no-decode -e GM -o "$TMP/no-decode.rkt" "$TMP/input.mes"
grep -q '(mll-load (string-bytes 115 121 115 116 101 109 46 109 108 108))' \
    "$TMP/no-decode.rkt"
grep -q '(text-raw 1 24 25 4 130 164)' "$TMP/no-decode.rkt"
"$JUICE" -c -f -o "$TMP/no-decode.mes" "$TMP/no-decode.rkt"
cmp "$TMP/input.mes" "$TMP/no-decode.mes"

"$JUICE" -d -f --no-resolve -e GM -o "$TMP/no-resolve.rkt" "$TMP/input.mes"
grep -q '(cmd:110 "system.mll")' "$TMP/no-resolve.rkt"
grep -q '(cmd:64 (local-address 39) (address 30994) default)' \
    "$TMP/no-resolve.rkt"
"$JUICE" -c -f -o "$TMP/no-resolve.mes" "$TMP/no-resolve.rkt"
cmp "$TMP/input.mes" "$TMP/no-resolve.mes"

# The native opcode-0x40 convention stores a continuation, ends the current
# dispatch, and resumes at the immediately following label. Present that exact
# sequence as one editor-facing call and regenerate its continuation address.
printf '%b' '\x02\x00\x40\x09\x00\x34\x12\x00\x00\x4a\x02X\x00\x00' \
    > "$TMP/call.mes"
"$JUICE" -d -f -e GM -o "$TMP/call.rkt" "$TMP/call.mes"
grep -q '(call (address 4660))' "$TMP/call.rkt"
! grep -q '(local-address 9)' "$TMP/call.rkt"
! grep -q '(label 9)' "$TMP/call.rkt"
"$JUICE" -c -f -o "$TMP/call-output.mes" "$TMP/call.rkt"
cmp "$TMP/call.mes" "$TMP/call-output.mes"

# A type-1 IF frame whose target follows while-continue is the native GM loop
# encoding. Lift the frame/label mechanics into the same while form used by the
# other Juice engines and lower it back byte-for-byte.
printf '%b' '\x02\x00\x33\x0a\x00\x01\x01\x00\x66\x38\x00' \
    > "$TMP/while.mes"
"$JUICE" -d -f -e GM -o "$TMP/while.rkt" "$TMP/while.mes"
grep -q '(while 1' "$TMP/while.rkt"
grep -q '(wait-key)' "$TMP/while.rkt"
! grep -q '(while-continue)' "$TMP/while.rkt"
! grep -q '(label 10)' "$TMP/while.rkt"
"$JUICE" -c -f -o "$TMP/while-output.mes" "$TMP/while.rkt"
cmp "$TMP/while.mes" "$TMP/while-output.mes"

# Undecodable SJIS/gaiji pairs remain local escapes inside otherwise editable
# text instead of forcing the complete record into text-raw.
printf '%b' '\x04\x00\xeb\xa0\x4a\x01\x18\x82\xa0\x00\x00' \
    > "$TMP/mixed-text.mes"
"$JUICE" -d -f -e GM -o "$TMP/mixed-text.rkt" "$TMP/mixed-text.mes"
grep -q '(text (chr-raw 235 160) "あ")' "$TMP/mixed-text.rkt"
! grep -q '(text-raw ' "$TMP/mixed-text.rkt"
"$JUICE" -c -f -o "$TMP/mixed-text-output.mes" "$TMP/mixed-text.rkt"
cmp "$TMP/mixed-text.mes" "$TMP/mixed-text-output.mes"

# Be-Yond's retail 0xff wrapper is selected in metadata, auto-detected on
# input, and deterministically regenerated.
sed "s/(charset \"pc98\")/(charset \"pc98\") (compression 'beyond)/" \
    "$TMP/output.rkt" > "$TMP/packed-source.rkt"
"$JUICE" -c -f -o "$TMP/packed.mes" "$TMP/packed-source.rkt"
test "$(od -An -tu1 -N1 "$TMP/packed.mes" | tr -d ' ')" = 255
"$JUICE" -d -f --auto-engine -o "$TMP/packed.rkt" "$TMP/packed.mes"
grep -q "(compression 'beyond)" "$TMP/packed.rkt"
cmp "$TMP/packed-source.rkt" "$TMP/packed.rkt"
"$JUICE" -c -f -o "$TMP/packed-again.mes" "$TMP/packed.rkt"
cmp "$TMP/packed.mes" "$TMP/packed-again.mes"

"$JUICE" -c -f --auto-wrap -o "$TMP/wrapped.mes" "$TMP/output.rkt" \
    > "$TMP/wrapped.log" 2>&1 || true
test ! -e "$TMP/wrapped.mes"
grep -q -- '--auto-wrap is not supported for GM scripts' "$TMP/wrapped.log"

# Every known base opcode and Be-Yond extension has a hand-written semantic
# representative. Compile it, decompile it without raw fallback, and demand a
# byte-exact second compilation.
"$JUICE" -c -f -o "$TMP/semantic.mes" "$ROOT/tests/fixtures/gm-semantic.rkt"
"$JUICE" -d -f -e GM -o "$TMP/semantic.rkt" "$TMP/semantic.mes"
! grep -q '(raw ' "$TMP/semantic.rkt"
grep -q '(for-end ' "$TMP/semantic.rkt"
grep -q '(push-reference ' "$TMP/semantic.rkt"
grep -q '(pop-reference ' "$TMP/semantic.rkt"
"$JUICE" -c -f -o "$TMP/semantic-roundtrip.mes" "$TMP/semantic.rkt"
cmp "$TMP/semantic.mes" "$TMP/semantic-roundtrip.mes"

# GM files cannot exceed the engine's 16-bit file-address space.
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
