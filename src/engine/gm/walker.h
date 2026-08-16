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

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gm {

struct InstructionSpan {
    size_t start;
    size_t end;
    uint8_t opcode;
};

struct LocalRelocation {
    size_t field;
    uint16_t target;
};

struct WalkResult {
    std::vector<InstructionSpan> instructions;
    std::vector<LocalRelocation> relocations;
};

// Decode the complete GM instruction stream. All returned positions are
// absolute MES file offsets, including the dictionary/header prefix.
WalkResult walk_code(const std::vector<uint8_t>& code, size_t code_base);

} // namespace gm
