#!/bin/bash
# Synthetic General Message detection and byte-exact round-trip test.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
JUICE="${1:-$ROOT/build/juice}"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/lime-juice-gm.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

# Header: two dictionary entries (あ, い). Code contains the observed GM
# SYSTEM.MLL loader signature, mode-1 dictionary/raw text, and mode-2 ASCII.
printf '%b' '\x06\x00\x82\xa0\x82\xa2\x6e\x11system.mll\x00\x00\x31\x00\x4a\x01\x18\x19\x04\x82\xa4\x00\x50\x00\x4a\x02BS\x00\x00' > "$TMP/input.mes"

"$JUICE" -d -f --auto-engine -o "$TMP/output.rkt" "$TMP/input.mes"
grep -q "(engine 'GM)" "$TMP/output.rkt"
grep -q '(gm-text 1 "あい\\nう")' "$TMP/output.rkt"
grep -q '(gm-text 2 "BS")' "$TMP/output.rkt"

"$JUICE" -c -f -o "$TMP/output.mes" "$TMP/output.rkt"
cmp "$TMP/input.mes" "$TMP/output.mes"

echo "GM synthetic round-trip: passed"
