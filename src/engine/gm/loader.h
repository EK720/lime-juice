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

#include <string>

namespace gm {

// General Message is only partially understood. The loader decodes its
// self-delimiting text opcode and preserves every other byte in (raw ...)
// nodes, so a decompile/compile cycle remains lossless.
AstNode load_mes(const std::string& path, Config& cfg);

} // namespace gm
