//
// lime-juice: C++ port of Tomyun's "Juice" de/recompiler for PC-98 games
// Copyright (C) 2026 Fuzion
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//

#include "loader.h"
#include "opener.h"
#include "walker.h"
#include "../../charset.h"
#include "../../utf8.h"

#include <iterator>
#include <optional>
#include <vector>

namespace gm {

namespace {

struct TextRecord {
    size_t end;
    int mode;
    std::string text;
};

bool is_sjis_pair(uint8_t lead, uint8_t trail) {
    bool valid_lead = (lead >= 0x81 && lead <= 0x9f) ||
                      (lead >= 0xe0 && lead <= 0xef);
    bool valid_trail = trail >= 0x40 && trail <= 0xfc && trail != 0x7f;
    return valid_lead && valid_trail;
}

std::optional<TextRecord> parse_text(const std::vector<uint8_t>& code, size_t start,
                                     const std::vector<std::vector<int>>& dict,
                                     const Charset& cs) {
    if (start + 2 >= code.size() || code[start] != 0x4a ||
        (code[start + 1] != 1 && code[start + 1] != 2)) {
        return std::nullopt;
    }

    int mode = code[start + 1];
    size_t pos = start + 2;
    std::string text;

    while (pos < code.size() && code[pos] != 0) {
        uint8_t byte = code[pos];

        if (mode == 2) {
            if (byte == 0x04) {
                text.push_back('\n');
                pos++;
                continue;
            }

            if (byte < 0x20 || byte > 0x7e) {
                return std::nullopt;
            }

            text.push_back(static_cast<char>(byte));
            pos++;
            continue;
        }

        if (byte == 0x04) {
            text.push_back('\n');
            pos++;
            continue;
        }

        int dict_index = -1;

        if (byte >= 0x18 && byte <= 0x7f) {
            dict_index = byte - 0x18;
        } else if (byte >= 0xa0 && byte <= 0xdf) {
            dict_index = byte - 0x38;
        }

        std::vector<int> sjis;

        if (dict_index >= 0) {
            if (dict_index >= static_cast<int>(dict.size())) {
                return std::nullopt;
            }

            sjis = dict[dict_index];
            pos++;
        } else {
            if (pos + 1 >= code.size() || !is_sjis_pair(byte, code[pos + 1])) {
                return std::nullopt;
            }

            sjis = {byte, code[pos + 1]};
            pos += 2;
        }

        auto decoded = cs.sjis_to_char(sjis);

        if (!decoded.has_value()) {
            return std::nullopt;
        }

        text += char32_to_utf8(*decoded);
    }

    if (pos >= code.size()) {
        return std::nullopt;
    }

    return TextRecord{pos + 1, mode, std::move(text)};
}

AstNode make_dict(const std::vector<std::vector<int>>& dict, const Config& cfg,
                  const Charset& cs) {
    std::vector<AstNode> entries;

    for (const auto& pair : dict) {
        auto decoded = cs.sjis_to_char(pair);

        if (cfg.decode && decoded.has_value()) {
            entries.push_back(AstNode::make_character(*decoded));
        } else {
            entries.push_back(AstNode::make_quote(AstNode::make_list("_sjis_", {
                AstNode::make_integer(pair[0]), AstNode::make_integer(pair[1])
            })));
        }
    }

    return AstNode::make_list("dict", std::move(entries));
}

void append_span(std::vector<AstNode>& layout, size_t start, size_t end) {
    layout.push_back(AstNode::make_list("span", {
        AstNode::make_integer(static_cast<int32_t>(start)),
        AstNode::make_integer(static_cast<int32_t>(end))
    }));
}

void flush_raw(std::vector<AstNode>& nodes, std::vector<AstNode>& raw,
               std::vector<AstNode>& layout, size_t start, size_t end) {
    if (!raw.empty()) {
        nodes.push_back(AstNode::make_list("raw", std::move(raw)));
        append_span(layout, start, end);
        raw.clear();
    }
}

} // namespace

AstNode load_mes(const std::string& path, Config& cfg) {
    Charset cs;
    cs.load(cfg.charset_name);
    auto mes = open_mes(path);

    std::vector<AstNode> nodes;
    nodes.push_back(AstNode::make_list("meta", {
        AstNode::make_list("engine", {
            AstNode::make_quote(AstNode::make_symbol("GM"))
        }),
        AstNode::make_list("charset", {
            AstNode::make_string(cfg.charset_name)
        })
    }));
    nodes.push_back(make_dict(mes.dictionary, cfg, cs));

    size_t code_base = 2 + mes.dictionary.size() * 2;
    auto walk = walk_code(mes.code, code_base);
    std::vector<AstNode> code_nodes;
    std::vector<AstNode> layout;
    std::vector<AstNode> raw;
    size_t raw_start = 0;
    size_t raw_end = 0;

    for (const auto& instruction : walk.instructions) {
        size_t start = instruction.start - code_base;
        size_t end = instruction.end - code_base;
        auto text = instruction.opcode == 0x4a
            ? parse_text(mes.code, start, mes.dictionary, cs)
            : std::nullopt;

        if (!text.has_value() || text->end != end || !cfg.decode) {
            if (raw.empty()) {
                raw_start = instruction.start;
            }

            for (size_t pos = start; pos < end; pos++) {
                raw.push_back(AstNode::make_integer(mes.code[pos]));
            }

            raw_end = instruction.end;
            continue;
        }

        flush_raw(code_nodes, raw, layout, raw_start, raw_end);
        code_nodes.push_back(AstNode::make_list("gm-text", {
            AstNode::make_integer(text->mode),
            AstNode::make_string(text->text)
        }));
        append_span(layout, instruction.start, instruction.end);
    }

    flush_raw(code_nodes, raw, layout, raw_start, raw_end);

    for (const auto& relocation : walk.relocations) {
        layout.push_back(AstNode::make_list("reloc", {
            AstNode::make_integer(static_cast<int32_t>(relocation.field)),
            AstNode::make_integer(relocation.target)
        }));
    }

    nodes.push_back(AstNode::make_list("gm-layout", std::move(layout)));
    nodes.insert(nodes.end(), std::make_move_iterator(code_nodes.begin()),
                 std::make_move_iterator(code_nodes.end()));
    return AstNode::make_list("mes", std::move(nodes));
}

} // namespace gm
