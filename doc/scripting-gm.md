# General Message scripting notes

General Message (GM) is a PC-98 scripting engine with a file container that
looks like AI5 but an incompatible instruction set. A valid GM file begins
with a little-endian 16-bit code offset. The bytes between offset 2 and the
code offset are two-byte Shift-JIS dictionary entries.

The interpreter dispatches opcodes `0x30` through `0x7f`. Most instruction
layouts are not identified yet, so juice preserves unparsed code byte-for-byte
in `(raw ...)` nodes. Do not edit those nodes unless you know the instruction
layout and its address operands.

## Text (`0x4a`)

Text records are self-delimiting and can therefore be edited safely:

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
single-byte ASCII directly.

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

Juice only gives semantic structure to `0x4a` text records today. It scans for
records whose complete payload is structurally valid and leaves every other
byte untouched. This permits translation and exact recompilation without
claiming that the surrounding control flow is understood. Future opcode
parsers can replace individual `(raw ...)` regions incrementally.
