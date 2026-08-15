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
#include "../../config.h"

#include <cstdint>
#include <vector>

namespace gm {

std::vector<uint8_t> compile_mes(const AstNode& ast, Config& cfg);

} // namespace gm
