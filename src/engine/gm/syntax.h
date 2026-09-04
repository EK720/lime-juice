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

#include <vector>

namespace gm {

// Fuse exact instruction sequences into the editor-facing GM syntax. The
// semantic decoder remains one-node-per-instruction; this pass only removes
// bytecode mechanics whose inverse lowering is unambiguous.
std::vector<AstNode> fuse_syntax(std::vector<AstNode> nodes);

// Expand editor-facing structured forms back into the exact flat instruction
// sequence consumed by the semantic encoder.
std::vector<AstNode> lower_syntax(const std::vector<AstNode>& nodes);

} // namespace gm
