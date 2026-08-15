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

#include "../engine.h"

#include <string>
#include <vector>

namespace gm {

MesFile open_mes(const std::string& path);
MesFile open_mes_bytes(const std::vector<uint8_t>& bytes);

} // namespace gm
