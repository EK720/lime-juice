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

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>

namespace gm {

namespace {

constexpr size_t kBeyondRingSize = 0x800;
constexpr size_t kBeyondMaxMatch = 0x100;

uint16_t read_u16_le(const std::vector<uint8_t>& bytes, size_t offset) {
    if (offset + 2 > bytes.size()) {
        throw std::runtime_error("truncated Be-Yond packed header");
    }

    return static_cast<uint16_t>(bytes[offset] | (bytes[offset + 1] << 8));
}

void append_u16_le(std::vector<uint8_t>& bytes, size_t value) {
    if (value > std::numeric_limits<uint16_t>::max()) {
        throw std::runtime_error("Be-Yond packed field exceeds 16 bits");
    }

    bytes.push_back(static_cast<uint8_t>(value));
    bytes.push_back(static_cast<uint8_t>(value >> 8));
}

OpenedMes open_unpacked_mes(const std::vector<uint8_t>& bytes,
                            bool beyond_packed) {
    if (bytes.size() < 2) {
        throw std::runtime_error("GM MES file too small (< 2 bytes)");
    }

    uint16_t offset = static_cast<uint16_t>(bytes[0] | (bytes[1] << 8));

    if (offset < 2 || offset > bytes.size() || (offset - 2) % 2 != 0) {
        throw std::runtime_error("invalid GM MES dictionary offset");
    }

    OpenedMes opened;
    opened.beyond_packed = beyond_packed;

    for (size_t i = 2; i < offset; i += 2) {
        opened.mes.dictionary.push_back({bytes[i], bytes[i + 1]});
    }

    opened.mes.code.assign(bytes.begin() + offset, bytes.end());
    return opened;
}

} // namespace

bool is_beyond_packed(const std::vector<uint8_t>& bytes) {
    return bytes.size() >= 9 && bytes[0] == 0xff;
}

std::vector<uint8_t> unpack_beyond(const std::vector<uint8_t>& bytes) {
    if (!is_beyond_packed(bytes)) {
        throw std::runtime_error("not a Be-Yond 0xff-packed payload");
    }

    size_t unpacked_size = read_u16_le(bytes, 1);
    size_t token_bytes = read_u16_le(bytes, 3);
    size_t token_count = read_u16_le(bytes, 5);
    size_t flag_bytes = read_u16_le(bytes, 7);
    size_t flags_start = 9;
    size_t tokens_start = flags_start + flag_bytes;

    if (tokens_start > bytes.size() || token_bytes > bytes.size() - tokens_start) {
        throw std::runtime_error("truncated Be-Yond packed payload");
    }

    if (token_count > flag_bytes * 8) {
        throw std::runtime_error("Be-Yond packed flag stream is too short");
    }
    if (flag_bytes != (token_count + 7) / 8) {
        throw std::runtime_error("Be-Yond packed flag byte count does not match");
    }
    if (tokens_start + token_bytes != bytes.size()) {
        throw std::runtime_error("trailing bytes after Be-Yond packed payload");
    }

    std::array<uint8_t, kBeyondRingSize> ring{};
    size_t ring_write = 0;
    size_t token_offset = 0;
    std::vector<uint8_t> output;
    output.reserve(unpacked_size);

    for (size_t index = 0; index < token_count; index++) {
        uint8_t flags = bytes[flags_start + index / 8];
        bool match = ((flags >> (7 - index % 8)) & 1) != 0;

        if (match) {
            if (token_offset + 3 > token_bytes) {
                throw std::runtime_error("Be-Yond match overruns token stream");
            }

            size_t source = static_cast<size_t>(
                bytes[tokens_start + token_offset] |
                (bytes[tokens_start + token_offset + 1] << 8));
            size_t length = static_cast<size_t>(
                bytes[tokens_start + token_offset + 2]) + 1;
            token_offset += 3;

            if (output.size() > unpacked_size ||
                length > unpacked_size - output.size()) {
                throw std::runtime_error("Be-Yond match exceeds unpacked size");
            }

            std::array<uint8_t, kBeyondMaxMatch> chunk{};
            for (size_t i = 0; i < length; i++) {
                chunk[i] = ring[(source + i) & (kBeyondRingSize - 1)];
            }
            for (size_t i = 0; i < length; i++) {
                output.push_back(chunk[i]);
                ring[ring_write] = chunk[i];
                ring_write = (ring_write + 1) & (kBeyondRingSize - 1);
            }
        } else {
            if (token_offset >= token_bytes || output.size() >= unpacked_size) {
                throw std::runtime_error("Be-Yond literal overruns packed stream");
            }

            uint8_t value = bytes[tokens_start + token_offset++];
            output.push_back(value);
            ring[ring_write] = value;
            ring_write = (ring_write + 1) & (kBeyondRingSize - 1);
        }
    }

    if (token_offset != token_bytes) {
        throw std::runtime_error("Be-Yond packed token byte count does not match");
    }
    if (output.size() != unpacked_size) {
        throw std::runtime_error("Be-Yond unpacked size does not match header");
    }

    return output;
}

std::vector<uint8_t> pack_beyond(const std::vector<uint8_t>& bytes) {
    if (bytes.size() > std::numeric_limits<uint16_t>::max()) {
        throw std::runtime_error("Be-Yond unpacked payload exceeds 16 bits");
    }

    std::array<uint8_t, kBeyondRingSize> ring{};
    std::array<std::set<uint16_t>, 256> positions_by_byte;
    for (size_t position = 0; position < kBeyondRingSize; position++) {
        positions_by_byte[0].insert(static_cast<uint16_t>(position));
    }

    std::vector<uint8_t> flags;
    std::vector<uint8_t> tokens;
    size_t ring_write = 0;
    size_t input_offset = 0;
    size_t token_count = 0;
    uint8_t current_flags = 0;
    size_t flag_bits = 0;

    while (input_offset < bytes.size()) {
        size_t limit = std::min(kBeyondMaxMatch, bytes.size() - input_offset);
        size_t best_length = 0;
        uint16_t best_position = 0;

        for (uint16_t position : positions_by_byte[bytes[input_offset]]) {
            size_t length = 1;
            while (length < limit &&
                   ring[(position + length) & (kBeyondRingSize - 1)] ==
                       bytes[input_offset + length]) {
                length++;
            }

            if (length > best_length) {
                best_length = length;
                best_position = position;
            }
        }

        size_t length = 1;
        if (best_length >= 3) {
            length = best_length;
            current_flags |= static_cast<uint8_t>(1u << (7 - flag_bits));
            append_u16_le(tokens, best_position);
            tokens.push_back(static_cast<uint8_t>(length - 1));
        } else {
            tokens.push_back(bytes[input_offset]);
        }

        for (size_t i = 0; i < length; i++) {
            uint8_t value = bytes[input_offset + i];
            positions_by_byte[ring[ring_write]].erase(
                static_cast<uint16_t>(ring_write));
            ring[ring_write] = value;
            positions_by_byte[value].insert(static_cast<uint16_t>(ring_write));
            ring_write = (ring_write + 1) & (kBeyondRingSize - 1);
        }

        input_offset += length;
        token_count++;
        flag_bits++;
        if (flag_bits == 8) {
            flags.push_back(current_flags);
            current_flags = 0;
            flag_bits = 0;
        }
    }

    if (flag_bits != 0) {
        flags.push_back(current_flags);
    }

    std::vector<uint8_t> packed;
    packed.reserve(9 + flags.size() + tokens.size());
    packed.push_back(0xff);
    append_u16_le(packed, bytes.size());
    append_u16_le(packed, tokens.size());
    append_u16_le(packed, token_count);
    append_u16_le(packed, flags.size());
    packed.insert(packed.end(), flags.begin(), flags.end());
    packed.insert(packed.end(), tokens.begin(), tokens.end());
    return packed;
}

OpenedMes open_mes(const std::string& path) {
    std::ifstream file(path, std::ios::binary);

    if (!file.is_open()) {
        throw std::runtime_error("cannot open file: " + path);
    }

    std::vector<uint8_t> bytes(
        (std::istreambuf_iterator<char>(file)),
        std::istreambuf_iterator<char>());
    return open_mes_bytes(bytes);
}

OpenedMes open_mes_bytes(const std::vector<uint8_t>& bytes) {
    if (is_beyond_packed(bytes)) {
        return open_unpacked_mes(unpack_beyond(bytes), true);
    }

    return open_unpacked_mes(bytes, false);
}

} // namespace gm
