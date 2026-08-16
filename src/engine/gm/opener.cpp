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

#include "opener.h"

#include <fstream>
#include <stdexcept>

namespace gm {

MesFile open_mes(const std::string& path) {
    std::ifstream file(path, std::ios::binary);

    if (!file.is_open()) {
        throw std::runtime_error("cannot open file: " + path);
    }

    std::vector<uint8_t> bytes(
        (std::istreambuf_iterator<char>(file)),
        std::istreambuf_iterator<char>());
    return open_mes_bytes(bytes);
}

MesFile open_mes_bytes(const std::vector<uint8_t>& bytes) {
    if (bytes.size() < 2) {
        throw std::runtime_error("GM MES file too small (< 2 bytes)");
    }

    uint16_t offset = static_cast<uint16_t>(bytes[0] | (bytes[1] << 8));

    if (offset < 2 || offset > bytes.size() || (offset - 2) % 2 != 0) {
        throw std::runtime_error("invalid GM MES dictionary offset");
    }

    MesFile mes;

    for (size_t i = 2; i < offset; i += 2) {
        mes.dictionary.push_back({bytes[i], bytes[i + 1]});
    }

    mes.code.assign(bytes.begin() + offset, bytes.end());
    return mes;
}

} // namespace gm
