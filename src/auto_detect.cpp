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

#include "auto_detect.h"
#include "engine/gm/opener.h"
#include "engine/gm/walker.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <string>

static bool valid_sjis_pair(uint8_t lead, uint8_t trail) {
    bool valid_lead = (lead >= 0x81 && lead <= 0x9f) ||
                      (lead >= 0xe0 && lead <= 0xef);
    bool valid_trail = trail >= 0x40 && trail <= 0xfc && trail != 0x7f;
    return valid_lead && valid_trail;
}

// Return the byte after a structurally valid, self-delimiting GM text record.
// This deliberately does not decode text; auto-detection must work without a
// selected charset.
static size_t gm_text_end(const std::vector<uint8_t>& bytes, size_t start,
                          size_t dict_entries) {
    if (start + 2 >= bytes.size() || bytes[start] != 0x4a ||
        (bytes[start + 1] != 1 && bytes[start + 1] != 2)) {
        return 0;
    }

    int mode = bytes[start + 1];
    size_t pos = start + 2;

    while (pos < bytes.size() && bytes[pos] != 0) {
        uint8_t byte = bytes[pos];

        if (mode == 2) {
            if (byte == 0x04) {
                pos++;
                continue;
            }

            if (!((byte >= 0x20 && byte <= 0x7e) ||
                  (byte >= 0xa1 && byte <= 0xdf))) {
                return 0;
            }

            pos++;
        } else if (byte == 0x04) {
            pos++;
        } else if (byte >= 0x18 && byte <= 0x7f) {
            if (static_cast<size_t>(byte - 0x18) >= dict_entries) {
                return 0;
            }

            pos++;
        } else if (byte >= 0xa0 && byte <= 0xdf) {
            if (static_cast<size_t>(byte - 0x38) >= dict_entries) {
                return 0;
            }

            pos++;
        } else {
            if (pos + 1 >= bytes.size() || !valid_sjis_pair(byte, bytes[pos + 1])) {
                return 0;
            }

            pos += 2;
        }
    }

    return pos < bytes.size() ? pos + 1 : 0;
}

static bool has_gm_startup_signature(const std::vector<uint8_t>& bytes,
                                     size_t code_start) {
    // Fermion's entry scenario begins by loading SYSTEM.MLL:
    //   6e 11 "system.mll" 00 00
    if (code_start + 7 >= bytes.size() || bytes[code_start] != 0x6e ||
        bytes[code_start + 1] != 0x11) {
        return false;
    }

    size_t end = code_start + 2;

    while (end < bytes.size() && end - code_start <= 64 && bytes[end] != 0) {
        if (bytes[end] < 0x20 || bytes[end] > 0x7e) {
            return false;
        }

        end++;
    }

    if (end + 1 >= bytes.size() || bytes[end] != 0 || bytes[end + 1] != 0) {
        return false;
    }

    std::string name(bytes.begin() + code_start + 2, bytes.begin() + end);
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return name.size() >= 4 && name.substr(name.size() - 4) == ".mll";
}

static bool looks_like_gm(const std::vector<uint8_t>& bytes, size_t code_start,
                          size_t dict_entries) {
    bool signature = has_gm_startup_signature(bytes, code_start);

    int records = 0;

    for (size_t pos = code_start; pos + 2 < bytes.size();) {
        size_t end = gm_text_end(bytes, pos, dict_entries);

        if (end != 0) {
            // Empty text commands are valid, but too weak to identify an
            // engine on their own.
            if (end > pos + 3) {
                records++;
            }

            pos = end;
        } else {
            pos++;
        }
    }

    if (!signature && records < 2) return false;

    try {
        std::vector<uint8_t> code(bytes.begin() + code_start, bytes.end());
        auto walk = gm::walk_code(code, code_start);
        return !walk.instructions.empty();
    } catch (const std::exception&) {
        return false;
    }
}

// score a byte sequence for AI5 vs AI1 structural markers.
// scans the given range and returns positive if AI5 markers dominate,
// negative if AI1 markers dominate.
// works entirely heuristically (vibes), so false positives are possible
// but should be rare.
static int score_engine_markers(const std::vector<uint8_t>& bytes,
                                size_t start, size_t end) {
    int ai5 = 0;
    int ai1 = 0;

    for (size_t i = start; i < end; i++) {
        uint8_t b = bytes[i];

        // AI5: 0x04 (SYS) is always followed by a system function number.
        // this 2-byte pattern is highly distinctive to AI5.

        if (b == 0x04 && i + 1 < end) {
            ai5 += 5;
            i++;
            continue;
        }

        // AI5: 0x03 (VAL) terminates expressions, appears very frequently

        if (b == 0x03) {
            ai5 += 2;
            continue;
        }

        // AI5: 0x06 (STR) string delimiter

        if (b == 0x06) {
            ai5 += 3;
            continue;
        }

        // AI5: 0x0F (CND) conditional

        if (b == 0x0F) {
            ai5 += 2;
            continue;
        }

        // AI1: 0x7B (BEG) block begin

        if (b == 0x7B) {
            ai1 += 3;
            continue;
        }

        // AI1: 0x7D (END) block end

        if (b == 0x7D) {
            ai1 += 3;
            continue;
        }

        // AI1: 0x9D (CND) conditional, very distinctive

        if (b == 0x9D) {
            ai1 += 4;
            continue;
        }

        // AI1: 0x22 (STR) string delimiter

        if (b == 0x22) {
            ai1 += 2;
            continue;
        }
    }

    return ai5 - ai1;
}

// detect engine type from raw MES file bytes
EngineType detect_engine(const std::vector<uint8_t>& bytes) {

    if (bytes.size() < 2) {
        return EngineType::AI1;
    }

    // Retail Be-Yond wraps its ordinary GM MES payload in a 0xff-prefixed
    // token stream. The wrapper plus a complete structural walk is a much
    // stronger signature than the heuristics used for uncompressed files.
    if (gm::is_beyond_packed(bytes)) {
        try {
            auto opened = gm::open_mes_bytes(bytes);
            size_t code_base = 2 + opened.mes.dictionary.size() * 2;
            auto walk = gm::walk_code(opened.mes.code, code_base);
            if (!walk.instructions.empty()) return EngineType::GM;
        } catch (const std::exception&) {
            // Continue with the normal engine checks for malformed 0xff data.
        }
    }

    // check ADV: FF FE (end-of-mes marker), possibly followed by null padding

    size_t end = bytes.size();

    while (end > 0 && bytes[end - 1] == 0x00) {
        end--;
    }

    if (end >= 2 && bytes[end - 2] == 0xFF && bytes[end - 1] == 0xFE) {
        return EngineType::ADV;
    }

    // check AI5: first 2 bytes form a valid dictionary offset (LE uint16).
    // offset == 2 means empty dictionary, which is valid.
    // (offset - 2) must be even since the dictionary consists of byte pairs.
    uint16_t offset = static_cast<uint16_t>(bytes[0] | (bytes[1] << 8));

    if (offset >= 2 && offset <= bytes.size() && (offset - 2) % 2 == 0) {
        if (looks_like_gm(bytes, offset, (offset - 2) / 2)) {
            return EngineType::GM;
        }

        // the offset check alone produces false positives for AI1 files whose
        // first 2 bytes coincidentally form a valid-looking offset.
        // validate by scoring engine-specific structural byte patterns.
        // scan both the supposed code section AND from byte 0, since AI1 files
        // that produce large offset values may have text data (not code) at
        // the offset position, but will always have structural markers near
        // the start of the file.
        int score = 0;

        // scan the supposed code section (bytes from offset onward)
        size_t code_start = static_cast<size_t>(offset);
        size_t code_scan_end = std::min(bytes.size(), code_start + 512);

        if (code_start < bytes.size()) {
            score += score_engine_markers(bytes, code_start, code_scan_end);
        }

        // also scan from the beginning of the file.
        // for AI1, this is raw bytecode with structural markers (0x7B, 0x7D).
        // for AI5, this is the offset header + dictionary (high bytes, neutral score).
        size_t head_scan_end = std::min(bytes.size(), static_cast<size_t>(512));
        score += score_engine_markers(bytes, 0, head_scan_end);

        if (score < 0) {
            return EngineType::AI1;
        }

        return EngineType::AI5;
    }

    // fallback
    return EngineType::AI1;
}
