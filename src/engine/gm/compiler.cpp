//
// lime-juice: C++ port of Tomyun's "Juice" de/recompiler for PC-98 games
// Copyright (C) 2026 Fuzion
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//

#include "compiler.h"
#include "../../byte_writer.h"
#include "../../charset.h"
#include "../../utf8.h"

#include <stdexcept>
#include <unordered_map>

namespace gm {

namespace {

uint16_t pair_key(int first, int second) {
    return static_cast<uint16_t>((first << 8) | second);
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
    if (node.children.size() != 2 || !node.children[0].is_integer() ||
        !node.children[1].is_string()) {
        throw std::runtime_error("gm: expected (gm-text MODE \"text\")");
    }

    int mode = node.children[0].int_val;

    if (mode != 1 && mode != 2) {
        throw std::runtime_error("gm: text mode must be 1 or 2");
    }

    out.emit(0x4a);
    out.emit(static_cast<uint8_t>(mode));

    if (mode == 2) {
        for (unsigned char byte : node.children[1].str_val) {
            if (byte < 0x20 || byte > 0x7e) {
                throw std::runtime_error("gm: mode 2 text only supports printable ASCII");
            }

            out.emit(byte);
        }
    } else {
        std::unordered_map<uint16_t, size_t> lookup;

        for (size_t i = 0; i < dict.size(); i++) {
            lookup.emplace(pair_key(dict[i][0], dict[i][1]), i);
        }

        for (char32_t ch : utf8_to_codepoints(node.children[1].str_val)) {
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

} // namespace

std::vector<uint8_t> compile_mes(const AstNode& ast, Config& cfg) {
    for (const auto& node : ast.children) {
        if (!node.is_list("meta")) {
            continue;
        }

        for (const auto& entry : node.children) {
            if (entry.is_list("charset") && !entry.children.empty() &&
                entry.children[0].is_string()) {
                cfg.charset_name = entry.children[0].str_val;
            }
        }

        break;
    }

    Charset cs;
    cs.load(cfg.charset_name);
    auto dict = read_dict(ast, cs);

    ByteWriter out;
    emit_header(out, dict);

    for (const auto& node : ast.children) {
        if (node.is_list("meta") || node.is_list("dict")) {
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
        } else {
            throw std::runtime_error("gm compiler: unsupported node: " + node.tag);
        }
    }

    return out.take_data();
}

} // namespace gm
