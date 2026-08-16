#!/bin/bash
# Synthetic General Message detection and byte-exact round-trip test.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
JUICE="${1:-$ROOT/build/juice}"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/lime-juice-gm.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

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

echo "GM synthetic round-trip and relocation: passed"
