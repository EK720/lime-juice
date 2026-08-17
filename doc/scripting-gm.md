# General Message scripting reference

General Message (GM) is the PC-98 script engine used by *Fermion: Mirai kara
no Houmonsha* and *Be-Yond: Kurodaishou ni Mirareteru*. Juice supports the
system-1 Rev.95:06:30 instruction set used by both games, including Be-Yond's
six extended commands and its retail `0xff` compression wrapper.

## Quick start

Decompile an unpacked Fermion file or a packed Be-Yond file directly:

```sh
juice -d --auto-engine SCENE.MES
# or choose the game explicitly
juice -d -p fermion SCENE.MES
juice -d -p beyond SCENE.MES
```

Edit the generated `SCENE.rkt`, then compile it:

```sh
juice -c SCENE.rkt
```

A decompiled Be-Yond script carries `(compression 'beyond)` in its metadata,
so the ordinary compile command writes another game-ready packed file. The
`beyond` preset also enables compression for hand-written source.

## File structure

A typical source file begins like this:

```racket
(mes
 (meta (engine 'GM) (charset "pc98") (compression 'beyond))
 (dict #\あ #\い #\う)
 (gm-mll-load "system.mll")
 (gm-text 1 "あいう")
 (gm-message-end)
 (gm-end))
```

Remove `(compression 'beyond)` for unpacked output. Fermion source normally
has only the `engine` and `charset` metadata entries.

The unpacked binary starts with a little-endian 16-bit code offset. Bytes
between offset 2 and the code offset are two-byte Shift-JIS dictionary entries.
All code addresses include that header and dictionary prefix.

## Common edits

Dialogue is ordinary text:

```racket
(gm-text 1 "日本語の台詞")
(gm-text 2 "ASCII text")
```

Mode 1 uses the file dictionary and raw two-byte Shift-JIS characters. Mode 2
accepts printable ASCII. In either mode, `\n` compiles to the engine's `0x04`
newline control.

Resource names and other printable byte-string parameters are quoted strings:

```racket
(gm-image-open "IMAGE.GPC")
(gm-mes-jump "NEXT.MES")
(gm-mll-load "SYSTEM.MLL")
```

Complete commands may be inserted, deleted, or reordered. Local control
targets use labels, so their encoded addresses are recalculated after an edit.

## Commands and unresolved form

Known opcodes use semantic names such as `gm-if`, `gm-assign`, `gm-text-color`,
`gm-mouse-command`, and `gm-save-slot`. The encodable fixture
`tests/fixtures/gm-semantic.rkt` is the compact reference for every supported
opcode and operand shape.

`--no-resolve` keeps typed operands but writes numeric command names:

```racket
(cmd:110 "SYSTEM.MLL") ; opcode 0x6e
```

Numeric `cmd:N` nodes compile normally. The four native no-operations follow
the existing Juice convention: `nop:48`, `nop:54`, `nop:66`, and `nop:127`.

| Range | Area | Representative nodes |
|---|---|---|
| `0x30`-`0x42` | control flow and menus | `gm-for-start`, `gm-if`, `gm-gosub-if`, `gm-menu`, `gm-return` |
| `0x43`-`0x4f` | assignment and text | `gm-assign`, `gm-string-copy`, `gm-text-window`, `gm-text` |
| `0x50`-`0x63` | palette and graphics | `gm-message-end`, `gm-palette-set`, `gm-image-open`, `gm-blit` |
| `0x64`-`0x71` | input, files, and drivers | `gm-input`, `gm-mouse-command`, `gm-mes-jump`, `gm-mll-load`, `gm-video-command` |
| `0x72`-`0x7f` | save state and resources | `gm-file-save-range`, `gm-save-slot`, `gm-image-load`, `gm-music-command` |
| `0x80`-`0x85` | Be-Yond extensions | `gm-beyond-flag-test`, `gm-for-end`, `gm-push-reference` |

## Expressions

The binary stores expressions in postfix order. Juice presents a balanced
stream as a conventional nested tree while retaining literal widths:

```racket
(gm-expr
 (== (& (gm-ref 12 86) (gm-imm 1 32))
     (gm-imm 1 0)))
```

The binary operators are `*`, `/`, `%`, `+`, `-`, `&`, `|`, `==`, `!=`, `<`,
`<=`, `>`, `>=`, `&&`, and `||`.

- `(gm-imm WIDTH VALUE)` is an integer with its original 1- to 4-byte width.
- `(gm-ref TOKEN OFFSET [INDEX])` is a typed reference. Tokens 5 through 10
  carry a nested index expression.
- `(gm-random LOW HIGH)` is the initial random-range operand.
- `(gm-expr)` is the engine's empty/default expression.

Some shipped scripts contain a postfix stream that cannot form a tree, such as
an expression ending in an extra operator. Those rare cases remain flat and in
byte order:

```racket
(gm-expr (gm-imm 1 10) ==)
```

## Parameters and byte strings

Generic parameters can be expressions, strings, or a typed reference:

```racket
(gm-text-window
 (gm-expr (gm-imm 1 1))
 "AB"
 (gm-ref-param (gm-ref 15 600)))
```

Printable ASCII uses a quoted string. Arbitrary payloads use
`(gm-string-bytes BYTE ...)`, which is also what `--no-decode` emits. The
reference-parameter terminator used by Fermion is present throughout the
Be-Yond retail corpus too; the dialects share this parameter grammar.

## Text (`0x4a`)

Mode 1 payload tokens are:

| Bytes | Meaning |
|-------|---------|
| `04` | newline |
| `18`-`7f` | dictionary entry `token - 0x18` |
| `a0`-`df` | dictionary entry `token - 0x38` |
| otherwise | raw two-byte Shift-JIS character |

Mode 2 stores printable ASCII and uses the same `04` newline control. When mode
1 uses a valid but non-canonical byte spelling, Juice adds `gm-text-source`.
An unchanged node recompiles to its original bytes; appended text preserves
the source prefix and encodes only the suffix.

If character decoding is disabled or text cannot be represented in the
selected charset, Juice emits the exact payload:

```racket
(gm-text-raw 1 24 25 4 130 164)
```

`--no-decode` affects dictionary entries, text, and byte-string parameters. It
does not turn semantic commands into `(raw ...)` blocks.

## Labels and addresses

Local and external addresses are intentionally distinct:

```racket
(gm-gosub-if-save
 (gm-local-address 359)
 (gm-address 31010)
 (gm-expr))
(gm-end)
(gm-label 359)
(gm-text 1 "続き")
```

The `gm-label` number is an identifier copied from the input address; it is not
necessarily the address emitted after editing. At compile time, each
`gm-local-address` resolves to the current output position of its matching
label. This makes changed-length and structural edits safe.

`gm-address` is a literal external address, commonly an entry point in a loaded
MLL, and is never relocated. A missing or duplicate local label is a compile
error. Native targets must land on instruction boundaries, so labels always
appear between complete source nodes.

## Be-Yond retail compression

Retail Be-Yond `.MES` files use a `0xff` header followed by sizes, an MSB-first
flag stream, and literal or 2 KiB sliding-window match tokens. Juice detects,
unpacks, and validates the wrapper before walking the normal GM payload.

Compilation uses a deterministic greedy encoder. The unpacked MES semantics
round-trip exactly, and compiling the same RKT twice produces identical packed
bytes. A newly packed file may differ from the retail file because multiple
valid tokenizations exist.

## Be-Yond extensions

Be-Yond adds six commands beyond the Fermion table:

| Opcode | Node | Encoding and role |
|---|---|---|
| `0x80` | `gm-beyond-flag-test` | no operands; tests the engine helper flag |
| `0x81` | `gm-beyond-external-call` | parameter list; calls an external far service and stores `AX` |
| `0x82` | `gm-beyond-bank` | parameter list; selects a graphics bank |
| `0x83` | `gm-for-end` | loop id, local target, and expression |
| `0x84` | `gm-push-reference` | reference plus two posthook bytes |
| `0x85` | `gm-pop-reference` | reference plus two posthook bytes |

The tiny `0x84` and `0x85` handlers only set flags and return, but the dispatcher
tests those flags immediately and continues into native push/pop posthooks
before fetching another opcode. Those paths consume the reference and two
trailing bytes, so the complete static instruction is encoded as `ref + 2`.

Two base-table names have game-specific provenance. The `0x60` and `0x61`
`gm-window-blit-setup` names describe Fermion handlers; the corresponding
Be-Yond table slots are GDC data and neither opcode occurs in its MES corpus.
`gm-music-command` (`0x7a`) controls the Pirorin/Witch2 `.WM` music path.

## Validation and limits

Run the synthetic contract test with a built Juice binary:

```sh
tests/test_gm.sh build/juice
```

Run either game corpus, packed or unpacked, with:

```sh
tests/test_gm_corpus.sh /path/to/mes/files build/juice
```

The corpus test requires semantic output without raw instruction fallback. It
checks byte-exact output for unpacked files, deterministic canonical output for
packed files, and recompiles every file after growing all mode-1 text records.

GM uses 16-bit file addresses, so an unpacked compiled file cannot exceed
65,536 bytes. Be-Yond's 16-bit unpacked-size field further limits packed output
to 65,535 bytes. `--auto-wrap` is not supported for GM scripts; the compiler
rejects it explicitly instead of silently leaving text unchanged.
