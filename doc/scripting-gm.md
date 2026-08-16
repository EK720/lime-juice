# General Message scripting notes

General Message (GM) is a PC-98 scripting engine with a file container that
looks like AI5 but an incompatible instruction set. This implementation is
derived from and validated against *Fermion: Mirai kara no Houmonsha*'s General
Message system-1 Rev.95:06:30 dialect; other titles and revisions have not yet
been validated. A valid GM file begins with a little-endian 16-bit code offset.
The bytes between offset 2 and the code offset are two-byte Shift-JIS dictionary
entries.

The interpreter dispatches opcodes `0x30` through `0x7f`. Juice structurally
walks the complete instruction stream so it can distinguish instructions from
their payloads and identify local 16-bit control targets. Only text has a
semantic AST node today; every other complete instruction is preserved
byte-for-byte in `(raw ...)` nodes.

## Text (`0x4a`)

Text records are self-delimiting and can be edited safely in files decompiled
by a relocation-aware version of juice:

```racket
(gm-text 1 "Japanese text")
(gm-text 2 "ASCII")
```

The opcode is followed by a mode byte, the payload, and a zero terminator.

Mode 1 payload tokens are:

| Bytes | Meaning |
|-------|---------|
| `04` | newline |
| `18`-`7f` | dictionary entry `token - 0x18` |
| `a0`-`df` | dictionary entry `token - 0x38` |
| otherwise | raw two-byte Shift-JIS character |

The two dictionary ranges address up to 168 entries. Mode 2 stores printable
single-byte ASCII directly and uses the same `04` newline control as mode 1.

## Layout and relocation metadata

The decompiler writes a `gm-layout` node before the code nodes:

```racket
(gm-layout
 (span 6 26)
 (span 26 34)
 (span 34 39)
 (span 39 40)
 (reloc 21 39))
```

There is one source `span` for each following `raw` or `gm-text` node. A
`reloc` records the absolute file offset of a local 16-bit address field and
its old target. When text changes length, the compiler maps both positions
through the spans and backpatches the new target. Targets that do not land on
an instruction boundary in the same MES file, such as calls into an MLL
module, are deliberately not rewritten.

Keep `gm-layout` and the order of the code nodes intact. Text nodes may change
length. Raw nodes may be edited in place but cannot change length, because
juice does not yet have a semantic representation from which to rebuild them.
Older or hand-written GM source without `gm-layout` remains compilable, but it
does not have enough provenance for safe changed-length edits.

## Scenario loading

Three related commands have been identified:

| Opcode | Behavior |
|--------|----------|
| `0x6d` | replace the current MES scenario |
| `0x6e` | load an MLL module |
| `0x6f` | call a nested MES scenario and then restore the caller |

String parameters use token `0x11`, followed by a zero-terminated filename;
the parameter list itself ends with another zero. A common entry-script
signature is therefore `6e 11 "system.mll" 00 00`, which juice also uses as a
strong GM auto-detection marker.

## Current boundary

Juice only gives semantic structure to `0x4a` text records today. The remaining
opcode layouts are decoded only far enough to recover instruction boundaries
and native local address fields. This permits translation, exact unchanged
round trips, and changed-length relocation without claiming semantic names for
the surrounding commands. Future opcode parsers can replace individual
`(raw ...)` regions incrementally while retaining the same layout metadata.
The structural layouts and relocation fields exercised by Fermion are validated
across its corpus, but semantic opcode names beyond text and the
scenario-loading commands above remain future work.
