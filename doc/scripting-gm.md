# General Message scripting notes

General Message (GM) is a PC-98 scripting engine with a file container that
resembles AI5 but an incompatible instruction set. Juice supports the system-1
Rev.95:06:30 dialects used by *Fermion: Mirai kara no Houmonsha* and *Be-Yond:
Kurodaishou ni Mirareteru*. The implementation has been validated with
byte-exact round trips across Fermion's MES corpus and the unpacked Be-Yond MES
corpus. Be-Yond's `0xff` archive compression is a separate outer layer and must
be removed before passing a payload to juice.

A GM file begins with a little-endian 16-bit code offset. Bytes between offset
2 and the code offset are two-byte Shift-JIS dictionary entries. Fermion
dispatches opcodes `0x30` through `0x7f`; Be-Yond uses the same base table and
six extensions, `0x80` through `0x85`.

## Semantic form

Every known command has a named `gm-*` node. For example:

```racket
(gm-if
 (gm-address 1234)
 (gm-expr (gm-ref 12 86) (gm-imm 1 32) bit-and (gm-imm 1 0) eq))
(gm-assign
 (gm-ref 12 100)
 (gm-value (gm-expr (gm-imm 2 500))))
(gm-mll-load (gm-string-bytes 115 121 115 116 101 109 46 109 108 108))
```

Expressions preserve the engine's postfix term stream rather than imposing a
possibly incorrect higher-level tree:

- `(gm-imm WIDTH VALUE)` is a literal and retains its encoded width.
- `(gm-ref TOKEN OFFSET [INDEX-EXPR])` is a typed reference. Tokens `5` through
  `10` carry the nested index expression.
- `(gm-random LOW HIGH)` is the initial inclusive random-range operand.
- `mul`, `div`, `mod`, `add`, `sub`, `bit-and`, `bit-or`, `eq`, `ne`, `lt`,
  `le`, `gt`, `ge`, `logical-and`, and `logical-or` are native operators.
- `(gm-expr)` is the engine's empty/default expression.

Generic command arguments are represented as expressions,
`(gm-string-bytes ...)`, or `(gm-ref-param (gm-ref ...))`. Byte strings remain
explicit because filenames and driver payloads are engine bytes rather than
necessarily Unicode text.

The complete command vocabulary and an encodable representative of every base
opcode and Be-Yond extension live in `tests/fixtures/gm-semantic.rkt`.

Two base-table names have dialect-specific provenance. `gm-window-blit-setup`
and `gm-window-blit-setup-alt` (`0x60`/`0x61`) describe the Fermion handlers;
the corresponding Be-Yond table words are data and neither opcode occurs in
the Be-Yond corpus. `gm-music-command` (`0x7a`) controls the Pirorin/Witch2
music driver: observed selectors load/play or stop/start `.WM` music.

## Text (`0x4a`)

Text remains directly editable:

```racket
(gm-text 1 "Japanese text")
(gm-text 2 "ASCII")
```

Mode 1 payload tokens are:

| Bytes | Meaning |
|-------|---------|
| `04` | newline |
| `18`-`7f` | dictionary entry `token - 0x18` |
| `a0`-`df` | dictionary entry `token - 0x38` |
| otherwise | raw two-byte Shift-JIS character |

Mode 2 stores printable ASCII and uses the same `04` newline control. When a
source uses a non-canonical but valid spelling of the same text, juice adds a
`gm-text-source` annotation. An unchanged text node then recompiles to the
original bytes; appending text preserves the original prefix encoding.

## Layout and relocation metadata

The decompiler writes `gm-layout` before the code:

```racket
(gm-layout
 (span 6 20)
 (span 20 26)
 (span 26 34)
 (reloc 21 34))
```

There is one source span per following code node. Semantic control operands use
`(gm-address OLD-TARGET)`. During compilation their output fields are recorded
as they are emitted, then local targets are mapped from old spans to new spans.
This permits changed-length edits to text, expressions, parameter lists, and
other semantic commands. Addresses outside the MES code range, such as MLL
entry points, remain unchanged.

Keep `gm-layout` and code-node order intact when editing a decompiled file.
Hand-written source without layout metadata is also valid, but its numeric
addresses are emitted literally because there is no old-to-new mapping.

Legacy GM files containing `(raw ...)` nodes remain compilable. A raw node
covered by layout metadata cannot change length; new decompilation output uses
semantic nodes for every valid known instruction except undecodable text bytes.

## Be-Yond extensions

Be-Yond adds six commands beyond the Fermion table:

| Opcode | Node | Role |
|--------|------|------|
| `0x80` | `gm-beyond-flag-test` | test the engine flag used by its helper path |
| `0x81` | `gm-beyond-external-call` | call an external far service with parameters and store `AX` |
| `0x82` | `gm-beyond-bank` | choose and store a graphics bank |
| `0x83` | `gm-for-end` | conditional FOR-end variant |
| `0x84` | `gm-push-reference` | push a typed reference value |
| `0x85` | `gm-pop-reference` | pop and store a typed reference value |

The tiny `0x84` and `0x85` handlers only set flags and return, but the dispatcher
tests those flags immediately and continues into native push/pop posthooks
before fetching another opcode. Those posthooks consume the reference and two
trailing bytes, so each complete instruction is encoded as `ref + 2`.

## Validation

`tests/test_gm.sh` checks detection, semantic compilation, text edits,
relocation, postfix expressions ending in an operator, legacy raw
compatibility, and a synthetic representative of every known opcode slot.
`tests/test_gm_corpus.sh` performs byte-exact no-op round trips and grows every
mode-1 text record in a supplied corpus, then decompiles the changed result to
validate relocated instruction boundaries. Supply unpacked MES payloads; the
script does not unpack Be-Yond's `0xff` archive wrapper.
