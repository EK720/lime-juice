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
#include "../../config.h"

#include <cstddef>
#include <cstdint>
#include <vector>
#include <unordered_set>

namespace gm {

// Optionally return absolute operand fields explicitly marked external.
std::vector<uint8_t> compile_mes(
    const AstNode& ast, Config& cfg,
    std::unordered_set<size_t>* external_fields = nullptr);

} // namespace gm
