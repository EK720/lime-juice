//
// lime-juice: C++ port of Tomyun's "Juice" de/recompiler for PC-98 games
// Copyright (C) 2026 Fuzion
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//

#pragma once

#include "../../ast.h"
#include "../../byte_writer.h"
#include "walker.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gm {

// A control address emitted by a semantic instruction. target is the address
// from the source AST; the compiler remaps local targets after all nodes have
// been emitted, so expressions and text may change size safely.
struct SemanticRelocation {
    size_t field;
    uint16_t target;
};

// Decode one span previously validated by walk_code(). Text (0x4a) remains in
// loader.cpp because it needs the MES dictionary and selected charset.
AstNode decode_instruction(const std::vector<uint8_t>& code, size_t code_base,
                           const InstructionSpan& instruction);

// Emit one semantic GM node. Returns false when the tag does not belong to the
// GM semantic vocabulary. Appends relocations for address-bearing operands.
bool emit_instruction(ByteWriter& out, const AstNode& node,
                      std::vector<SemanticRelocation>& relocations);

// Stable command vocabulary used by both directions and by documentation.
const char* opcode_name(uint8_t opcode);

} // namespace gm
