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
#include "semantic.h"
#include "../../byte_writer.h"
#include "../../charset.h"
#include "../../utf8.h"

#include <stdexcept>
#include <optional>
#include <unordered_map>
#include <vector>

namespace gm {

namespace {

struct SourceSpan {
    size_t start;
    size_t end;
};

struct OutputSpan {
    size_t start;
    size_t end;
    bool semantic;
};

struct Relocation {
    size_t field;
    uint16_t target;
};

struct Layout {
    bool present = false;
    std::vector<SourceSpan> spans;
    std::vector<Relocation> relocations;
};

size_t layout_integer(const AstNode& node, const std::string& what,
                      size_t maximum) {
    if (!node.is_integer() || node.int_val < 0 ||
        static_cast<size_t>(node.int_val) > maximum) {
        throw std::runtime_error("gm: " + what + " is out of range");
    }

    return static_cast<size_t>(node.int_val);
}

Layout read_layout(const AstNode& ast) {
    Layout layout;

    for (const auto& node : ast.children) {
        if (!node.is_list("gm-layout")) {
            continue;
        }

        if (layout.present) {
            throw std::runtime_error("gm: multiple gm-layout nodes");
        }

        layout.present = true;

        for (const auto& entry : node.children) {
            if (entry.is_list("span") && entry.children.size() == 2) {
                size_t start = layout_integer(entry.children[0], "span start", 0xffff);
                size_t end = layout_integer(entry.children[1], "span end", 0x10000);

                if (start >= end) {
                    throw std::runtime_error("gm: layout span must be non-empty");
                }

                if (!layout.spans.empty() && layout.spans.back().end != start) {
                    throw std::runtime_error("gm: layout spans must be contiguous");
                }

                layout.spans.push_back({start, end});
            } else if (entry.is_list("reloc") && entry.children.size() == 2) {
                size_t field = layout_integer(entry.children[0], "relocation field", 0xffff);
                size_t target = layout_integer(entry.children[1], "relocation target", 0xffff);
                layout.relocations.push_back({field, static_cast<uint16_t>(target)});
            } else {
                throw std::runtime_error("gm: malformed gm-layout entry");
            }
        }
    }

    return layout;
}

size_t remap_position(size_t source, const std::vector<SourceSpan>& old_spans,
                      const std::vector<OutputSpan>& new_spans) {
    for (size_t i = 0; i < old_spans.size(); i++) {
        const auto& old_span = old_spans[i];

        if (source < old_span.start || source >= old_span.end) {
            continue;
        }

        size_t offset = source - old_span.start;
        size_t old_size = old_span.end - old_span.start;
        size_t new_size = new_spans[i].end - new_spans[i].start;

        if (old_size != new_size && offset != 0) {
            throw std::runtime_error(
                "gm: relocation points inside a resized source node");
        }

        if (offset >= new_size) {
            throw std::runtime_error("gm: relocation no longer fits its source node");
        }

        return new_spans[i].start + offset;
    }

    throw std::runtime_error("gm: relocation is outside the recorded source spans");
}

void apply_relocations(ByteWriter& out, const Layout& layout,
                       const std::vector<OutputSpan>& output_spans) {
    for (const auto& relocation : layout.relocations) {
        size_t source_index = layout.spans.size();

        for (size_t i = 0; i < layout.spans.size(); i++) {
            if (relocation.field >= layout.spans[i].start &&
                relocation.field < layout.spans[i].end) {
                source_index = i;
                break;
            }
        }

        if (source_index == layout.spans.size()) {
            throw std::runtime_error(
                "gm: relocation field is outside the recorded source spans");
        }

        // Semantic nodes record their fields while emitting and do not depend
        // on the old byte offset within a potentially resized instruction.
        if (output_spans[source_index].semantic) {
            continue;
        }

        size_t field = remap_position(relocation.field, layout.spans, output_spans);
        size_t field_end = remap_position(relocation.field + 1,
                                          layout.spans, output_spans);
        size_t target = remap_position(relocation.target,
                                       layout.spans, output_spans);

        if (field_end != field + 1) {
            throw std::runtime_error("gm: relocated control field is not contiguous");
        }

        if (target > 0xffff) {
            throw std::runtime_error("gm: relocated control target exceeds 16 bits");
        }

        out.write_u16_le_at(field, static_cast<uint16_t>(target));
    }
}

size_t remap_target(size_t target, const Layout& layout,
                    const std::vector<OutputSpan>& output_spans) {
    if (!layout.present) {
        return target;
    }

    for (const auto& span : layout.spans) {
        if (target >= span.start && target < span.end) {
            return remap_position(target, layout.spans, output_spans);
        }
    }

    return target;
}

void apply_semantic_relocations(
    ByteWriter& out, const std::vector<SemanticRelocation>& relocations,
    const Layout& layout, const std::vector<OutputSpan>& output_spans) {
    for (const auto& relocation : relocations) {
        size_t target = remap_target(relocation.target, layout, output_spans);

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
    auto layout = read_layout(ast);

    ByteWriter out;
    emit_header(out, dict);
    std::vector<OutputSpan> output_spans;
    std::vector<SemanticRelocation> semantic_relocations;
    size_t span_index = 0;

    for (const auto& node : ast.children) {
        if (node.is_list("meta") || node.is_list("dict") ||
            node.is_list("gm-layout")) {
            continue;
        }

        size_t output_start = out.size();
        bool semantic = false;

        if (node.is_list("raw")) {
            for (const auto& byte : node.children) {
                if (!byte.is_integer() || byte.int_val < 0 || byte.int_val > 255) {
                    throw std::runtime_error("gm: raw byte must be an integer from 0 to 255");
                }

                out.emit(static_cast<uint8_t>(byte.int_val));
            }
        } else if (node.is_list("gm-text")) {
            emit_text(out, node, cs, dict);
            semantic = true;
        } else if (emit_instruction(out, node, semantic_relocations)) {
            semantic = true;
        } else {
            throw std::runtime_error("gm compiler: unsupported node: " + node.tag);
        }

        size_t output_end = out.size();

        if (layout.present) {
            if (span_index >= layout.spans.size()) {
                throw std::runtime_error("gm: more code nodes than layout spans");
            }

            const auto& source = layout.spans[span_index];

            if (node.is_list("raw") &&
                output_end - output_start != source.end - source.start) {
                throw std::runtime_error("gm: raw nodes with layout metadata cannot change length");
            }

            output_spans.push_back({output_start, output_end, semantic});
            span_index++;
        }
    }

    if (layout.present && span_index != layout.spans.size()) {
        throw std::runtime_error("gm: fewer code nodes than layout spans");
    }

    if (out.size() > 0x10000) {
        throw std::runtime_error("gm: compiled file exceeds the 16-bit address space");
    }

    if (layout.present) {
        apply_relocations(out, layout, output_spans);
    }

    apply_semantic_relocations(out, semantic_relocations, layout, output_spans);

    return out.take_data();
}

} // namespace gm
