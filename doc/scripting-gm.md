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

For small or hand-written unpacked files without the usual `SYSTEM.MLL`
startup command or at least two non-empty text records, select GM explicitly
with `-e GM` or a game preset. Auto-detection stays conservative so that such
signature-less files are not mistaken for unrelated AI1/AI5 bytecode.

Edit the generated `SCENE.rkt`, then compile it:

```sh
juice -c SCENE.rkt
```

A decompiled Be-Yond script carries `(compression 'beyond)` in its metadata,
so the ordinary compile command writes another game-ready packed file. The
metadata entry is the explicit packing switch for both decompiled and
hand-written source; choosing a preset does not change the source's packing
mode.

## File structure

A typical source file begins like this:

```racket
(mes
 (meta (engine 'GM) (charset "pc98") (compression 'beyond))
 (dict #\あ #\い #\う)
 (mll-load "system.mll")
 (text "あいう")
 (message-end)
 (end))
```

Remove `(compression 'beyond)` for unpacked output. Fermion source normally
has only the `engine` and `charset` metadata entries.

The unpacked binary starts with a little-endian 16-bit code offset. Bytes
between offset 2 and the code offset are two-byte Shift-JIS dictionary entries.
All code addresses include that header and dictionary prefix.

## Common edits

Dialogue is ordinary text:

```racket
(text "日本語の台詞")
(text #:mode 2 "ASCII and ﾊﾝｶｸ text")
```

Mode 1 uses the file dictionary and raw two-byte Shift-JIS characters. Mode 2
accepts printable ASCII and JIS X 0201 halfwidth katakana. In either mode,
`\n` compiles to the engine's `0x04` newline control.

Resource names and other printable byte-string parameters are quoted strings:

```racket
(image-open "IMAGE.GPC")
(mes-jump "NEXT.MES")
(mll-load "SYSTEM.MLL")
(struct-assign (ref 15 0) "SCENE.GP4")
```

Complete commands may be inserted, deleted, or reordered. Local control
targets use labels, so their encoded addresses are recalculated after an edit.

## Commands and unresolved form

Dispatcher opcodes use descriptive names such as `if-frame`, `assign`, `text-color`,
`mouse-command`, and `save-slot`. As with the other engines, the `(engine 'GM)`
metadata scopes those names, so they do not carry an engine prefix. The
encodable fixture `tests/fixtures/gm-semantic.rkt` is the compact reference for
every supported opcode and operand shape.

All dispatcher slots have named nodes and typed encodings. Common `call` and
`while` conventions are lifted into structured forms; selector-driven I/O such
as `mouse-command`, `video-command`, and `music-command` remains represented as
typed selector and parameter lists.

`--no-resolve` keeps typed operands but writes numeric command names:

```racket
(cmd:110 "SYSTEM.MLL") ; opcode 0x6e
```

Numeric `cmd:N` nodes compile normally. The four native no-operations follow
the existing Juice convention: `nop:48`, `nop:54`, `nop:66`, and `nop:127`.
GM deliberately has no generic `(raw BYTE ...)` instruction form: every
command goes through its typed layout and relocation checks.

| Range | Area | Representative nodes |
|---|---|---|
| `0x30`-`0x42` | control flow and menus | `for-start`, `if-frame`, `gosub-if`, `menu`, `return` |
| `0x43`-`0x4f` | assignment and text | `assign`, `string-copy`, `text-window`, `text` |
| `0x50`-`0x63` | palette and graphics | `message-end`, `palette-set`, `image-open`, `blit` |
| `0x64`-`0x71` | input, files, and drivers | `input`, `mouse-command`, `mes-jump`, `mll-load`, `video-command` |
| `0x72`-`0x7f` | save state and resources | `file-save-range`, `save-slot`, `image-load`, `music-command` |
| `0x80`-`0x85` | Be-Yond extensions | `beyond-flag-test`, `for-end`, `push-reference` |

## Expressions

The binary stores expressions in postfix order. Juice presents a balanced
stream as the same conventional nested tree used by the other engines:

```racket
(== (& (ref 12 86) 32) 0)
```

The binary operators are `*`, `/`, `%`, `+`, `-`, `&`, `|`, `==`, `!=`, `<`,
`<=`, `>`, `>=`, `&&`, and `||`.

- Integers are ordinary numbers. The compiler selects the smallest native
  1- to 4-byte encoding that holds the value.
- `(ref TOKEN OFFSET [INDEX])` is a typed reference. Tokens 5 through 10
  carry a nested index expression.
- `(random LOW HIGH)` is the initial random-range operand.
- `default` is the engine's empty/default expression where an operand position
  cannot simply be omitted.

Some shipped scripts contain a postfix stream that cannot form a tree, such as
an expression ending in an extra operator. Those rare cases remain flat and in
byte order:

```racket
(postfix 10 ==)
```

## Parameters and byte strings

Generic parameters can be expressions, strings, or a typed reference:

```racket
(text-window
 1
 "AB"
 (ref-param (ref 15 600)))
```

Printable ASCII uses a quoted string. Arbitrary payloads use
`(string-bytes BYTE ...)`, which is also what `--no-decode` emits. All seven
`0x0f` reference parameters observed in the 253-file retail Be-Yond corpus use
the same reference-plus-zero encoding as Fermion.

## Text (`0x4a`)

Mode 1 payload tokens are:

| Bytes | Meaning |
|-------|---------|
| `04` | newline |
| `18`-`7f` | dictionary entry `token - 0x18` |
| `a0`-`df` | dictionary entry `token - 0x38` |
| otherwise | raw two-byte Shift-JIS character |

Mode 2 stores printable ASCII and JIS X 0201 bytes `a1`-`df`, presented as
Unicode halfwidth katakana `U+FF61`-`U+FF9F`. It uses the same `04` newline
control. Unknown Shift-JIS extensions and gaiji remain local escapes inside
otherwise editable text instead of making the whole line raw:

```racket
(text "known text" (chr-raw 235 160) "more text")
```

If an entire record cannot be represented canonically without changing its
original bytes, Juice retains that rare record as `text-raw`.

If character decoding is disabled or text cannot be represented in the
selected charset, Juice emits the exact payload:

```racket
(text-raw 1 24 25 4 130 164)
```

`--no-decode` affects dictionary entries, text, and byte-string parameters. It
does not turn semantic commands into `(raw ...)` blocks.

## Calls, loops, labels, and addresses

Common native control conventions are lifted automatically. An opcode `0x40`
call, its dispatcher terminator, and its private continuation label become one
node:

```racket
(call (address 31010))
(text "続き")
```

Likewise, the native IF-frame, `while-continue`, and exit label are presented as
a structured loop:

```racket
(while (< (ref 12 18) 50)
 (assign (ref 5 0 (ref 12 18)) 0)
 (assign (ref 12 18) (+ (ref 12 18) 1)))
```

The compiler regenerates both conventions and their internal continuation
addresses. Explicit labels remain only for control flow that is shared or
cannot be reduced without changing its meaning:

```racket
(if-frame (== (ref 11 1) 0) (local-address 500))
...
(label 500)
```

`if-frame` is the native type-1 IF-frame primitive, with its condition first
and skip target second. It is only exposed when the surrounding flow cannot be
lifted safely into a structured form.

The `label` number is an identifier copied from the input address; it is not
necessarily the address emitted after editing. At compile time, each
`local-address` resolves to the current output position of its matching label.
This makes changed-length and structural edits safe.

`address` is a literal external address, commonly an entry point in a loaded
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
| `0x80` | `beyond-flag-test` | no operands; tests the engine helper flag |
| `0x81` | `beyond-external-call` | parameter list; calls an external far service and stores `AX` |
| `0x82` | `beyond-bank` | parameter list; selects a graphics bank |
| `0x83` | `for-end` | loop id, local target, and expression |
| `0x84` | `push-reference` | reference plus two posthook bytes |
| `0x85` | `pop-reference` | reference plus two posthook bytes |

The tiny `0x84` and `0x85` handlers only set flags and return, but the dispatcher
tests those flags immediately and continues into native push/pop posthooks
before fetching another opcode. Those paths consume the reference and two
trailing bytes, so the complete static instruction is encoded as `ref + 2`.

Two base-table names have game-specific provenance. The `0x60` and `0x61`
`window-blit-setup` names describe Fermion handlers; the corresponding
Be-Yond table slots are GDC data and neither opcode occurs in its MES corpus.
`music-command` (`0x7a`) controls the Pirorin/Witch2 `.WM` music path.

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
The two retail corpora exercise 60 of the 80 base-table opcodes. Layouts for the
remaining base slots are derived from their native handlers but are unexercised
in retail scripts.

GM uses 16-bit file addresses, so an unpacked compiled file cannot exceed
65,536 bytes. Be-Yond's 16-bit unpacked-size field further limits packed output
to 65,535 bytes. `--auto-wrap` is not supported for GM scripts; the compiler
rejects it explicitly instead of silently leaving text unchanged.
