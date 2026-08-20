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
#include "syntax.h"
#include "walker.h"
#include "../../byte_writer.h"
#include "../../charset.h"
#include "../../utf8.h"

#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
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
                    "gm: local address has no matching label: " +
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

std::optional<uint8_t> mode2_byte(char32_t ch) {
    if (ch == U'\n') return 0x04;
    if (ch >= 0x20 && ch <= 0x7e) return static_cast<uint8_t>(ch);
    if (ch >= 0xff61 && ch <= 0xff9f) {
        return static_cast<uint8_t>(ch - 0xfec0);
    }
    return std::nullopt;
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
    int mode = 1;
    size_t first = 0;
    if (node.children.size() >= 2 && node.children[0].is_keyword() &&
        node.children[0].str_val == "mode" && node.children[1].is_integer()) {
        mode = node.children[1].int_val;
        first = 2;
    }
    if ((mode != 1 && mode != 2) || first >= node.children.size()) {
        throw std::runtime_error(
            "gm: expected (text [#:mode 2] \"text\" [(chr-raw B1 B2) ...])");
    }

    out.emit(0x4a);
    out.emit(static_cast<uint8_t>(mode));
    std::unordered_map<uint16_t, size_t> lookup;
    for (size_t i = 0; i < dict.size(); i++) {
        lookup.emplace(pair_key(dict[i][0], dict[i][1]), i);
    }

    auto emit_sjis = [&](int first_byte, int second_byte) {
        auto found = lookup.find(pair_key(first_byte, second_byte));
        if (found == lookup.end()) {
            out.emit(static_cast<uint8_t>(first_byte));
            out.emit(static_cast<uint8_t>(second_byte));
        } else if (found->second < 104) {
            out.emit(static_cast<uint8_t>(0x18 + found->second));
        } else {
            out.emit(static_cast<uint8_t>(0x38 + found->second));
        }
    };

    for (size_t i = first; i < node.children.size(); i++) {
        const auto& item = node.children[i];
        if (item.is_string()) {
            if (mode == 2) {
                for (char32_t ch : utf8_to_codepoints(item.str_val)) {
                    auto byte = mode2_byte(ch);
                    if (!byte.has_value()) {
                        throw std::runtime_error(
                            "gm: mode 2 text only supports printable ASCII, "
                            "halfwidth katakana, and newlines");
                    }
                    out.emit(*byte);
                }
                continue;
            }

            for (char32_t ch : utf8_to_codepoints(item.str_val)) {
                if (ch == U'\n') {
                    out.emit(0x04);
                    continue;
                }
                auto sjis = cs.char_to_sjis(ch);
                if (!sjis.has_value() || sjis->size() != 2) {
                    throw std::runtime_error(
                        "gm: mode 1 text requires double-byte SJIS characters");
                }
                emit_sjis((*sjis)[0], (*sjis)[1]);
            }
        } else if (mode == 1 && item.is_char_raw() &&
                   item.raw_bytes.size() == 2) {
            emit_sjis(item.raw_bytes[0], item.raw_bytes[1]);
        } else {
            throw std::runtime_error("gm: unsupported text item");
        }
    }

    out.emit(0x00);
}

void emit_text_raw(ByteWriter& out, const AstNode& node) {
    if (node.children.empty()) {
        throw std::runtime_error(
            "gm: expected (text-raw MODE BYTE ...)");
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
    bool beyond_compression = false;

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
    size_t code_base = out.size();
    std::vector<SemanticRelocation> semantic_relocations;
    std::unordered_map<uint16_t, size_t> labels;
    auto lowered = lower_syntax(ast.children);

    for (const auto& node : lowered) {
        if (node.is_list("meta") || node.is_list("dict")) {
            continue;
        }

        if (node.is_list("label")) {
            if (node.children.size() != 1) {
                throw std::runtime_error("gm: malformed label");
            }
            uint16_t label = static_cast<uint16_t>(ast_integer(
                node.children[0], "label", 0xffff));
            if (!labels.emplace(label, out.size()).second) {
                throw std::runtime_error("gm: duplicate label: " +
                                         std::to_string(label));
            }
            continue;
        }

        if (node.is_list("text")) {
            emit_text(out, node, cs, dict);
        } else if (node.is_list("text-raw")) {
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
    std::vector<uint8_t> code(data.begin() + code_base, data.end());
    std::unordered_set<size_t> known_external_fields;
    for (const auto& relocation : semantic_relocations) {
        if (!relocation.local) {
            known_external_fields.insert(relocation.field);
        }
    }
    (void)walk_code(code, code_base, known_external_fields);
    return beyond_compression ? pack_beyond(data) : data;
}

} // namespace gm
