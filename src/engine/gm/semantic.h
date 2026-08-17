//
// lime-juice: C++ port of Tomyun's "Juice" de/recompiler for PC-98 games
// Copyright (C) 2026 Fuzion
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
//

#pragma once

#include "../../ast.h"
#include "../../byte_writer.h"
#include "walker.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace gm {

// A control address emitted by a semantic instruction. target is the address
// from the source AST; the compiler remaps local targets after all nodes have
// been emitted, so expressions and text may change size safely.
struct SemanticRelocation {
    size_t field;
    uint16_t target;
    bool local;
};

// Decode one span previously validated by walk_code(). Text (0x4a) remains in
// loader.cpp because it needs the MES dictionary and selected charset.
AstNode decode_instruction(const std::vector<uint8_t>& code, size_t code_base,
                           const InstructionSpan& instruction,
                           const std::unordered_set<size_t>& local_fields,
                           bool resolve, bool decode_strings);

// Emit one semantic GM node. Returns false when the tag does not belong to the
// GM semantic vocabulary. Appends relocations for address-bearing operands.
bool emit_instruction(ByteWriter& out, const AstNode& node,
                      std::vector<SemanticRelocation>& relocations);

// Stable command vocabulary used by both directions and by documentation.
const char* opcode_name(uint8_t opcode);

} // namespace gm
