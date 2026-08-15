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

#include "sexp_writer.h"
#include "utf8.h"

#include <sstream>

std::string SexpWriter::format(const AstNode& node) {
    std::string result;
    write_node(result, node, 0);
    return result;
}

SexpWriter::Style SexpWriter::get_style(const std::string& tag) const {
    // from juice.rkt mes-style-table (lines 119-122)
    if (tag == "define-proc") {
        return Style::Define;
    }

    if (tag == "if-else" || tag == "if" || tag == "cond" || tag == "while" ||
        tag == "when" || tag == "slot" || tag == "seg") {
        return Style::If;
    }

    if (tag == "set-arr~" || tag == "set-arr~b" || tag == "set-arr~c" || tag == "set-arr~d" ||
        tag == "set-reg:" || tag == "set-reg::" || tag == "set-reg:d" || tag == "set-var") {
        return Style::Set;
    }

    if (tag == "text" || tag == "str" || tag == "text-raw" || tag == "gm-text") {
        return Style::Inline;
    }

    // structural containers: one statement per line
    if (tag == "mes" || tag == "gm-layout" || tag == "seg*" || tag == "loop" ||
        tag == "<>" || tag == "<*>" || tag == "</>" || tag == "<+>" ||
        tag == "/" || tag == "//" || tag == "+" ||
        tag == "repeat") {
        return Style::Block;
    }

    return Style::Default;
}

void SexpWriter::write_node(std::string& out, const AstNode& node, int indent) {
    switch (node.kind) {
        case AstNode::Kind::List:
            write_list(out, node, indent);
            break;
        default:
            write_atom(out, node);
            break;
    }
}

void SexpWriter::write_list(std::string& out, const AstNode& node, int indent) {
    if (node.children.empty()) {
        out += "(" + node.tag + ")";
        return;
    }

    // empty-tag lists render as (child1 child2 ...) without a tag prefix
    // used for cond pairs: ((condition) (body))
    bool empty_tag = node.tag.empty();

    // inline style: always single line regardless of width.
    // used for text/str nodes where readability benefits from
    // keeping the full content together on one line.
    Style style_check = empty_tag ? Style::Default : get_style(node.tag);
    bool force_inline = (style_check == Style::Inline);

    // check if everything fits on one line
    int total_width = estimate_width(node);

    if (force_inline || total_width + indent <= MAX_LINE_WIDTH) {
        // single line
        if (empty_tag) {
            out += "(";

            for (size_t i = 0; i < node.children.size(); i++) {

                if (i > 0) {
                    out += " ";
                }

                write_node(out, node.children[i], indent + 1);
            }
        } else {
            out += "(" + node.tag;

            for (const auto& child : node.children) {
                out += " ";
                write_node(out, child, indent + node.tag.size() + 2);
            }
        }

        out += ")";
        return;
    }

    // multi-line formatting
    Style style = empty_tag ? Style::Block : get_style(node.tag);
    int body_indent = indent + 1;

    if (empty_tag) {
        out += "(";
    } else {
        out += "(" + node.tag;
    }

    switch (style) {
        case Style::Define:
            // define-proc style: first arg on same line, rest indented by 1
            if (!node.children.empty()) {
                out += " ";
                write_node(out, node.children[0], indent + node.tag.size() + 2);

                for (size_t i = 1; i < node.children.size(); i++) {
                    out += "\n" + std::string(body_indent, ' ');
                    write_node(out, node.children[i], body_indent);
                }
            }
            break;

        case Style::If:
            // if/while style: first arg on same line (condition), rest indented by tag length + 1
            body_indent = indent + node.tag.size() + 2;

            if (!node.children.empty()) {
                out += " ";
                write_node(out, node.children[0], body_indent);

                for (size_t i = 1; i < node.children.size(); i++) {
                    out += "\n" + std::string(body_indent, ' ');
                    write_node(out, node.children[i], body_indent);
                }
            }
            break;

        case Style::Set:
            // set style: first two args on same line, rest indented
            body_indent = indent + node.tag.size() + 2;

            if (node.children.size() >= 2) {
                out += " ";
                write_node(out, node.children[0], body_indent);
                out += " ";
                write_node(out, node.children[1], body_indent);

                for (size_t i = 2; i < node.children.size(); i++) {
                    out += "\n" + std::string(body_indent, ' ');
                    write_node(out, node.children[i], body_indent);
                }
            } else if (!node.children.empty()) {
                out += " ";
                write_node(out, node.children[0], body_indent);
            }
            break;

        case Style::Block:
            // structural containers: one child per line
            if (empty_tag && !node.children.empty()) {
                write_node(out, node.children[0], body_indent);

                for (size_t i = 1; i < node.children.size(); i++) {
                    out += "\n" + std::string(body_indent, ' ');
                    write_node(out, node.children[i], body_indent);
                }
            } else {
                for (const auto& child : node.children) {
                    out += "\n" + std::string(body_indent, ' ');
                    write_node(out, child, body_indent);
                }
            }

            break;

        case Style::Default: {
            // greedy word-wrap: pack children onto lines until width exceeded
            int col;

            if (empty_tag && !node.children.empty()) {
                // empty-tag: first child on same line as paren
                write_node(out, node.children[0], body_indent);
                col = body_indent + estimate_width(node.children[0]);

                for (size_t i = 1; i < node.children.size(); i++) {
                    int child_w = estimate_width(node.children[i]);

                    if (col + 1 + child_w + 1 > MAX_LINE_WIDTH) {
                        out += "\n" + std::string(body_indent, ' ');
                        col = body_indent;
                    } else {
                        out += " ";
                        col += 1;
                    }

                    write_node(out, node.children[i], col);
                    col += child_w;
                }
            } else {
                // tagged: pack children after tag
                col = indent + node.tag.size() + 1; // "(" + tag

                for (size_t i = 0; i < node.children.size(); i++) {
                    int child_w = estimate_width(node.children[i]);

                    if (col + 1 + child_w + 1 > MAX_LINE_WIDTH && i > 0) {
                        out += "\n" + std::string(body_indent, ' ');
                        col = body_indent;
                    } else {
                        out += " ";
                        col += 1;
                    }

                    write_node(out, node.children[i], col);
                    col += child_w;
                }
            }

            break;
        }
    }

    out += ")";
}

void SexpWriter::write_atom(std::string& out, const AstNode& node) {
    switch (node.kind) {
        case AstNode::Kind::Integer:
            out += std::to_string(node.int_val);
            break;

        case AstNode::Kind::Variable:
            out += std::string(1, node.var_val);
            break;

        case AstNode::Kind::Symbol:
            out += node.str_val;
            break;

        case AstNode::Kind::String: {
            // write as quoted string with escapes
            out += "\"";

            for (char c : node.str_val) {

                switch (c) {
                    case '"':  out += "\\\""; break;
                    case '\\': out += "\\\\"; break;
                    case '\n': out += "\\n"; break;
                    case '\t': out += "\\t"; break;
                    default:   out += c; break;
                }
            }

            out += "\"";
            break;
        }

        case AstNode::Kind::Character:
            write_char_literal(out, node.char_val);
            break;

        case AstNode::Kind::CharRaw:
            // write as raw byte list
            out += "(chr-raw";

            for (uint8_t b : node.raw_bytes) {
                out += " " + std::to_string(b);
            }

            out += ")";
            break;

        case AstNode::Kind::DicRef:
            out += "(dic " + std::to_string(node.int_val) + ")";
            break;

        case AstNode::Kind::Cut:
            out += "(cut)";
            break;

        case AstNode::Kind::Keyword:
            out += "#:" + node.str_val;
            break;

        case AstNode::Kind::Boolean:
            out += node.bool_val ? "#t" : "#f";
            break;

        case AstNode::Kind::Quote:
            out += "'";

            if (!node.children.empty()) {
                write_node(out, node.children[0], 0);
            }

            break;

        default:
            out += "???";
            break;
    }
}

void SexpWriter::write_char_literal(std::string& out, char32_t c) {
    // racket character literal format
    switch (c) {
        case '\n':   out += "#\\newline"; return;
        case '\t':   out += "#\\tab"; return;
        case ' ':    out += "#\\space"; return;
        case '\b':   out += "#\\backspace"; return;
        case 0x3000: out += "#\\u3000"; return;  // ideographic space
        default: break;
    }

    out += "#\\";
    out += char32_to_utf8(c);
}

int SexpWriter::estimate_width(const AstNode& node) {
    if (!node.is_list()) {
        // rough estimate for atoms
        switch (node.kind) {
            case AstNode::Kind::Integer:
                return std::to_string(node.int_val).size();
            case AstNode::Kind::Variable:
                return 1;
            case AstNode::Kind::Symbol:
                return node.str_val.size();
            case AstNode::Kind::String:
                return node.str_val.size() + 2; // quotes
            case AstNode::Kind::Character:
                return 4; // #\x roughly
            case AstNode::Kind::Keyword:
                return node.str_val.size() + 2; // #:name
            case AstNode::Kind::Boolean:
                return 2; // #t or #f
            default:
                return 5;
        }
    }

    // list: tag + parens + spaces + children
    int width = node.tag.size() + 2; // ( tag )

    for (const auto& child : node.children) {
        width += 1 + estimate_width(child); // space + child
    }

    return width;
}
