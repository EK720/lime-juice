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

#include "loader.h"
#include "compiler.h"
#include "../../sexp_reader.h"

#include <fstream>
#include <sstream>
#include "opener.h"
#include "semantic.h"
#include "syntax.h"
#include "walker.h"
#include "../../charset.h"
#include "../../utf8.h"

#include <iterator>
#include <optional>
#include <unordered_set>
#include <vector>

namespace gm {

namespace {

struct TextRecord {
    size_t end;
    int mode;
    std::vector<AstNode> content;
    std::vector<uint8_t> bytes;
};

void append_text(std::vector<AstNode>& content, std::string& text) {
    if (!text.empty()) {
        content.push_back(AstNode::make_string(text));
        text.clear();
    }
}

bool is_sjis_pair(uint8_t lead, uint8_t trail) {
    bool valid_lead = (lead >= 0x81 && lead <= 0x9f) ||
                      (lead >= 0xe0 && lead <= 0xef);
    bool valid_trail = trail >= 0x40 && trail <= 0xfc && trail != 0x7f;
    return valid_lead && valid_trail;
}

std::optional<char32_t> mode2_codepoint(uint8_t byte) {
    if (byte == 0x04) return U'\n';
    if (byte >= 0x20 && byte <= 0x7e) return byte;
    if (byte >= 0xa1 && byte <= 0xdf) {
        return static_cast<char32_t>(byte) + 0xfec0;
    }
    return std::nullopt;
}

std::optional<uint8_t> mode2_byte(char32_t ch) {
    if (ch == U'\n') return 0x04;
    if (ch >= 0x20 && ch <= 0x7e) return static_cast<uint8_t>(ch);
    if (ch >= 0xff61 && ch <= 0xff9f) {
        return static_cast<uint8_t>(ch - 0xfec0);
    }
    return std::nullopt;
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
    size_t payload_start = pos;
    std::vector<AstNode> content;
    std::string text;

    while (pos < code.size() && code[pos] != 0) {
        uint8_t byte = code[pos];

        if (mode == 2) {
            auto decoded = mode2_codepoint(byte);
            if (!decoded.has_value()) return std::nullopt;
            text += char32_to_utf8(*decoded);
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

        if (decoded.has_value()) {
            text += char32_to_utf8(*decoded);
        } else {
            append_text(content, text);
            content.push_back(AstNode::make_char_raw(
                static_cast<uint8_t>(sjis[0]), static_cast<uint8_t>(sjis[1])));
        }
    }

    if (pos >= code.size()) {
        return std::nullopt;
    }

    append_text(content, text);
    if (content.empty()) content.push_back(AstNode::make_string(""));
    return TextRecord{pos + 1, mode, std::move(content),
                      std::vector<uint8_t>(code.begin() + payload_start,
                                           code.begin() + pos)};
}

std::optional<std::vector<uint8_t>> canonical_text_bytes(
    int mode, const std::vector<AstNode>& content,
    const std::vector<std::vector<int>>& dict, const Charset& cs) {
    std::vector<uint8_t> result;

    if (mode == 2) {
        for (const auto& item : content) {
            if (!item.is_string()) return std::nullopt;
            for (char32_t ch : utf8_to_codepoints(item.str_val)) {
                auto encoded = mode2_byte(ch);
                if (!encoded.has_value()) return std::nullopt;
                result.push_back(*encoded);
            }
        }
        return result;
    }

    auto append_sjis = [&](const std::vector<int>& sjis) {
        size_t index = dict.size();
        for (size_t i = 0; i < dict.size(); i++) {
            if (dict[i] == sjis) {
                index = i;
                break;
            }
        }

        if (index < dict.size() && index < 104) {
            result.push_back(static_cast<uint8_t>(0x18 + index));
        } else if (index < dict.size()) {
            result.push_back(static_cast<uint8_t>(0x38 + index));
        } else {
            result.push_back(static_cast<uint8_t>(sjis[0]));
            result.push_back(static_cast<uint8_t>(sjis[1]));
        }
    };

    for (const auto& item : content) {
        if (item.is_string()) {
            for (char32_t ch : utf8_to_codepoints(item.str_val)) {
                if (ch == U'\n') {
                    result.push_back(0x04);
                    continue;
                }

                auto sjis = cs.char_to_sjis(ch);
                if (!sjis.has_value() || sjis->size() != 2) {
                    return std::nullopt;
                }
                append_sjis(*sjis);
            }
        } else if (item.is_char_raw() && item.raw_bytes.size() == 2) {
            append_sjis({item.raw_bytes[0], item.raw_bytes[1]});
        } else {
            return std::nullopt;
        }
    }

    return result;
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

} // namespace

AstNode load_mes(const std::string& path, Config& cfg) {
    Charset cs;
    cs.load(cfg.charset_name);
    auto opened = open_mes(path);
    const auto& mes = opened.mes;

    std::vector<AstNode> nodes;
    std::vector<AstNode> metadata = {
        AstNode::make_list("engine", {
            AstNode::make_quote(AstNode::make_symbol("GM"))
        }),
        AstNode::make_list("charset", {
            AstNode::make_string(cfg.charset_name)
        })
    };
    if (opened.beyond_packed) {
        metadata.push_back(AstNode::make_list("compression", {
            AstNode::make_quote(AstNode::make_symbol("beyond"))
        }));
    }
    nodes.push_back(AstNode::make_list("meta", std::move(metadata)));
    nodes.push_back(make_dict(mes.dictionary, cfg, cs));

    size_t code_base = 2 + mes.dictionary.size() * 2;
    std::unordered_set<size_t> external_fields;
    if (!cfg.gm_source.empty()) {
        std::ifstream source(cfg.gm_source);
        if (!source) throw std::runtime_error("gm: cannot open source context: " + cfg.gm_source);
        std::ostringstream text;
        text << source.rdbuf();
        SexpReader reader;
        auto ast = reader.parse(text.str());
        if (!ast.is_list("mes")) throw std::runtime_error("gm: source context must be a mes form");
        Config source_cfg = cfg;
        auto expected = open_mes_bytes(compile_mes(ast, source_cfg, &external_fields));
        // Packing choices do not affect the addresses in the unpacked payload.
        if (expected.mes.dictionary != mes.dictionary || expected.mes.code != mes.code) {
            throw std::runtime_error("gm: source context does not reproduce this MES payload");
        }
    }
    auto walk = walk_code(mes.code, code_base, external_fields);
    std::unordered_set<size_t> local_fields;
    std::unordered_set<size_t> local_targets;

    for (const auto& relocation : walk.relocations) {
        local_fields.insert(relocation.field);
        local_targets.insert(relocation.target);
    }

    for (const auto& instruction : walk.instructions) {
        size_t start = instruction.start - code_base;
        size_t end = instruction.end - code_base;

        if (local_targets.count(instruction.start) != 0) {
            nodes.push_back(AstNode::make_list("label", {
                AstNode::make_integer(static_cast<int32_t>(instruction.start))
            }));
        }

        auto text = cfg.decode && instruction.opcode == 0x4a
            ? parse_text(mes.code, start, mes.dictionary, cs)
            : std::nullopt;

        auto canonical = text.has_value()
            ? canonical_text_bytes(text->mode, text->content, mes.dictionary, cs)
            : std::nullopt;
        if (instruction.opcode == 0x4a && text.has_value() &&
            text->end == end && canonical.has_value() &&
            *canonical == text->bytes) {
            std::vector<AstNode> children;
            if (text->mode == 2) {
                children.push_back(AstNode::make_keyword("mode"));
                children.push_back(AstNode::make_integer(2));
            }
            children.insert(children.end(),
                            std::make_move_iterator(text->content.begin()),
                            std::make_move_iterator(text->content.end()));
            nodes.push_back(AstNode::make_list("text", std::move(children)));
        } else if (instruction.opcode == 0x4a) {
            std::vector<AstNode> children = {
                AstNode::make_integer(mes.code[start + 1])
            };
            for (size_t pos = start + 2; pos + 1 < end; pos++) {
                children.push_back(AstNode::make_integer(mes.code[pos]));
            }
            nodes.push_back(AstNode::make_list("text-raw",
                                               std::move(children)));
        } else {
            nodes.push_back(decode_instruction(mes.code, code_base,
                                               instruction, local_fields,
                                               cfg.resolve, cfg.decode));
        }
    }

    return AstNode::make_list("mes", fuse_syntax(std::move(nodes)));
}

} // namespace gm
