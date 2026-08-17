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

#include "compiler.h"
#include "opener.h"
#include "semantic.h"
#include "../../byte_writer.h"
#include "../../charset.h"
#include "../../utf8.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace gm {

namespace {

size_t ast_integer(const AstNode& node, const std::string& what,
                   size_t maximum) {
    if (!node.is_integer() || node.int_val < 0 ||
        static_cast<size_t>(node.int_val) > maximum) {
        throw std::runtime_error("gm: " + what + " is out of range");
    }

    return static_cast<size_t>(node.int_val);
}

void apply_semantic_relocations(
    ByteWriter& out, const std::vector<SemanticRelocation>& relocations,
    const std::unordered_map<uint16_t, size_t>& labels) {
    for (const auto& relocation : relocations) {
        size_t target = relocation.target;

        if (relocation.local) {
            auto label = labels.find(relocation.target);
            if (label == labels.end()) {
                throw std::runtime_error(
                    "gm: local address has no matching gm-label: " +
                    std::to_string(relocation.target));
            }
            target = label->second;
        }

        if (target > 0xffff) {
            throw std::runtime_error("gm: relocated semantic target exceeds 16 bits");
        }

        out.write_u16_le_at(relocation.field, static_cast<uint16_t>(target));
    }
}

uint16_t pair_key(int first, int second) {
    return static_cast<uint16_t>((first << 8) | second);
}

std::optional<std::string> decode_text_source(
    int mode, const std::vector<uint8_t>& bytes,
    const std::vector<std::vector<int>>& dict, const Charset& cs) {
    std::string text;

    if (mode == 2) {
        for (uint8_t value : bytes) {
            if (value == 0x04) {
                text.push_back('\n');
            } else if (value >= 0x20 && value <= 0x7e) {
                text.push_back(static_cast<char>(value));
            } else {
                return std::nullopt;
            }
        }
        return text;
    }

    for (size_t pos = 0; pos < bytes.size();) {
        uint8_t value = bytes[pos++];

        if (value == 0x04) {
            text.push_back('\n');
            continue;
        }

        int index = -1;
        if (value >= 0x18 && value <= 0x7f) index = value - 0x18;
        else if (value >= 0xa0 && value <= 0xdf) index = value - 0x38;
        std::vector<int> sjis;

        if (index >= 0) {
            if (index >= static_cast<int>(dict.size())) return std::nullopt;
            sjis = dict[index];
        } else {
            if (pos >= bytes.size()) return std::nullopt;
            sjis = {value, bytes[pos++]};
        }

        auto decoded = cs.sjis_to_char(sjis);
        if (!decoded.has_value()) return std::nullopt;
        text += char32_to_utf8(*decoded);
    }

    return text;
}

std::vector<std::vector<int>> read_dict(const AstNode& ast, const Charset& cs) {
    std::vector<std::vector<int>> dict;

    for (const auto& node : ast.children) {
        if (!node.is_list("dict")) {
            continue;
        }

        for (const auto& entry : node.children) {
            if (entry.is_character()) {
                auto sjis = cs.char_to_sjis(entry.char_val);

                if (!sjis.has_value() || sjis->size() != 2) {
                    throw std::runtime_error("gm: dictionary character is not double-byte SJIS");
                }

                dict.push_back(*sjis);
                continue;
            }

            const AstNode* raw = &entry;

            if (entry.is_quote() && !entry.children.empty()) {
                raw = &entry.children[0];
            }

            if (!raw->is_list("_sjis_") || raw->children.size() != 2 ||
                !raw->children[0].is_integer() || !raw->children[1].is_integer()) {
                throw std::runtime_error("gm: malformed dictionary entry");
            }

            if (raw->children[0].int_val < 0 || raw->children[0].int_val > 255 ||
                raw->children[1].int_val < 0 || raw->children[1].int_val > 255) {
                throw std::runtime_error("gm: dictionary bytes must be from 0 to 255");
            }

            dict.push_back({raw->children[0].int_val, raw->children[1].int_val});
        }

        break;
    }

    if (dict.size() > 168) {
        throw std::runtime_error("gm: dictionary exceeds the 168 encodable entries");
    }

    return dict;
}

void emit_header(ByteWriter& out, const std::vector<std::vector<int>>& dict) {
    uint16_t offset = static_cast<uint16_t>(2 + dict.size() * 2);
    out.emit_u16_le(offset);

    for (const auto& pair : dict) {
        out.emit(static_cast<uint8_t>(pair[0]));
        out.emit(static_cast<uint8_t>(pair[1]));
    }
}

void emit_text(ByteWriter& out, const AstNode& node, const Charset& cs,
               const std::vector<std::vector<int>>& dict) {
    if ((node.children.size() != 2 && node.children.size() != 3) ||
        !node.children[0].is_integer() ||
        !node.children[1].is_string()) {
        throw std::runtime_error(
            "gm: expected (gm-text MODE \"text\" [SOURCE])");
    }

    int mode = node.children[0].int_val;

    if (mode != 1 && mode != 2) {
        throw std::runtime_error("gm: text mode must be 1 or 2");
    }

    out.emit(0x4a);
    out.emit(static_cast<uint8_t>(mode));
    std::string text = node.children[1].str_val;

    if (node.children.size() == 3) {
        const auto& source = node.children[2];

        if (!source.is_list("gm-text-source") || source.children.empty() ||
            !source.children[0].is_string()) {
            throw std::runtime_error("gm: malformed gm-text-source");
        }

        const std::string& original = source.children[0].str_val;
        std::vector<uint8_t> source_bytes;

        for (size_t i = 1; i < source.children.size(); i++) {
            if (!source.children[i].is_integer() ||
                source.children[i].int_val < 0 ||
                source.children[i].int_val > 255) {
                throw std::runtime_error("gm: invalid gm-text-source byte");
            }
            source_bytes.push_back(static_cast<uint8_t>(source.children[i].int_val));
        }

        auto decoded = decode_text_source(mode, source_bytes, dict, cs);

        if (decoded.has_value() && *decoded == original &&
            text.compare(0, original.size(), original) == 0) {
            out.emit(source_bytes);
            text.erase(0, original.size());
        }
    }

    if (mode == 2) {
        for (unsigned char byte : text) {
            if (byte == '\n') {
                out.emit(0x04);
                continue;
            }

            if (byte < 0x20 || byte > 0x7e) {
                throw std::runtime_error("gm: mode 2 text only supports printable ASCII and newlines");
            }

            out.emit(byte);
        }
    } else {
        std::unordered_map<uint16_t, size_t> lookup;

        for (size_t i = 0; i < dict.size(); i++) {
            lookup.emplace(pair_key(dict[i][0], dict[i][1]), i);
        }

        for (char32_t ch : utf8_to_codepoints(text)) {
            if (ch == U'\n') {
                out.emit(0x04);
                continue;
            }

            auto sjis = cs.char_to_sjis(ch);

            if (!sjis.has_value() || sjis->size() != 2) {
                throw std::runtime_error("gm: mode 1 text requires double-byte SJIS characters");
            }

            auto found = lookup.find(pair_key((*sjis)[0], (*sjis)[1]));

            if (found == lookup.end()) {
                out.emit(static_cast<uint8_t>((*sjis)[0]));
                out.emit(static_cast<uint8_t>((*sjis)[1]));
            } else if (found->second < 104) {
                out.emit(static_cast<uint8_t>(0x18 + found->second));
            } else {
                out.emit(static_cast<uint8_t>(0x38 + found->second));
            }
        }
    }

    out.emit(0x00);
}

void emit_text_raw(ByteWriter& out, const AstNode& node) {
    if (node.children.empty()) {
        throw std::runtime_error(
            "gm: expected (gm-text-raw MODE BYTE ...)");
    }

    int mode = ast_integer(node.children[0], "raw text mode", 255);
    if (mode != 1 && mode != 2) {
        throw std::runtime_error("gm: raw text mode must be 1 or 2");
    }

    out.emit(0x4a);
    out.emit(static_cast<uint8_t>(mode));
    for (size_t i = 1; i < node.children.size(); i++) {
        out.emit(static_cast<uint8_t>(ast_integer(
            node.children[i], "raw text byte", 255)));
    }
    out.emit(0);
}

} // namespace

std::vector<uint8_t> compile_mes(const AstNode& ast, Config& cfg) {
    std::string preset = cfg.preset;
    std::transform(preset.begin(), preset.end(), preset.begin(),
                   [](unsigned char ch) {
                       return static_cast<char>(std::tolower(ch));
                   });
    bool beyond_compression = preset == "beyond" || preset == "be-yond";

    for (const auto& node : ast.children) {
        if (!node.is_list("meta")) {
            continue;
        }

        for (const auto& entry : node.children) {
            if (entry.is_list("charset") && !entry.children.empty() &&
                entry.children[0].is_string()) {
                cfg.charset_name = entry.children[0].str_val;
            } else if (entry.is_list("compression")) {
                if (entry.children.size() != 1 ||
                    !entry.children[0].is_quote() ||
                    entry.children[0].children.size() != 1 ||
                    !entry.children[0].children[0].is_symbol("beyond")) {
                    throw std::runtime_error(
                        "gm: compression must be 'beyond");
                }
                beyond_compression = true;
            }
        }

        break;
    }

    Charset cs;
    cs.load(cfg.charset_name);
    auto dict = read_dict(ast, cs);
    ByteWriter out;
    emit_header(out, dict);
    std::vector<SemanticRelocation> semantic_relocations;
    std::unordered_map<uint16_t, size_t> labels;

    for (const auto& node : ast.children) {
        if (node.is_list("meta") || node.is_list("dict")) {
            continue;
        }

        if (node.is_list("gm-label")) {
            if (node.children.size() != 1) {
                throw std::runtime_error("gm: malformed gm-label");
            }
            uint16_t label = static_cast<uint16_t>(ast_integer(
                node.children[0], "label", 0xffff));
            if (!labels.emplace(label, out.size()).second) {
                throw std::runtime_error("gm: duplicate gm-label: " +
                                         std::to_string(label));
            }
            continue;
        }

        if (node.is_list("raw")) {
            for (const auto& byte : node.children) {
                if (!byte.is_integer() || byte.int_val < 0 || byte.int_val > 255) {
                    throw std::runtime_error("gm: raw byte must be an integer from 0 to 255");
                }

                out.emit(static_cast<uint8_t>(byte.int_val));
            }
        } else if (node.is_list("gm-text")) {
            emit_text(out, node, cs, dict);
        } else if (node.is_list("gm-text-raw")) {
            emit_text_raw(out, node);
        } else if (!emit_instruction(out, node, semantic_relocations)) {
            throw std::runtime_error("gm compiler: unsupported node: " + node.tag);
        }
    }

    if (out.size() > 0x10000) {
        throw std::runtime_error("gm: compiled file exceeds the 16-bit address space");
    }

    apply_semantic_relocations(out, semantic_relocations, labels);

    auto data = out.take_data();
    return beyond_compression ? pack_beyond(data) : data;
}

} // namespace gm
