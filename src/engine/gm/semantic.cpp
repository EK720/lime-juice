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

#include "semantic.h"

#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace gm {

namespace {

AstNode integer(uint32_t value) {
    return AstNode::make_integer(static_cast<int32_t>(value));
}

AstNode list(const std::string& tag, std::vector<AstNode> children = {}) {
    return AstNode::make_list(tag, std::move(children));
}

const char* operator_name(uint8_t token) {
    switch (token) {
        case 0x20: return "*";
        case 0x21: return "/";
        case 0x22: return "%";
        case 0x23: return "+";
        case 0x24: return "-";
        case 0x25: return "&";
        case 0x26: return "|";
        case 0x28: return "==";
        case 0x29: return "!=";
        case 0x2a: return "<";
        case 0x2b: return "<=";
        case 0x2c: return ">";
        case 0x2d: return ">=";
        case 0x2e: return "&&";
        case 0x2f: return "||";
        default: return nullptr;
    }
}

bool literal_value(const AstNode& expression, uint32_t& value) {
    if (!expression.is_integer()) {
        return false;
    }

    value = static_cast<uint32_t>(expression.int_val);
    return true;
}

class Decoder {
public:
    Decoder(const std::vector<uint8_t>& code, size_t code_base,
            const InstructionSpan& instruction,
            const std::unordered_set<size_t>& local_fields,
            bool resolve, bool decode_strings)
        : code_(code), base_(code_base), end_(instruction.end - code_base),
          pos_(instruction.start - code_base + 1),
          opcode_(instruction.opcode), local_fields_(local_fields),
          resolve_(resolve), decode_strings_(decode_strings) {
        if (instruction.start < code_base || instruction.end < instruction.start ||
            end_ > code.size()) {
            throw std::runtime_error("gm: invalid semantic instruction span");
        }
    }

    AstNode decode() {
        AstNode node = decode_body();

        if (pos_ != end_) {
            throw std::runtime_error("gm: semantic decoder did not consume instruction");
        }

        return node;
    }

private:
    uint8_t byte() {
        if (pos_ >= end_) {
            throw std::runtime_error("gm: truncated semantic operand");
        }

        return code_[pos_++];
    }

    uint8_t peek() const {
        if (pos_ >= end_) {
            throw std::runtime_error("gm: truncated semantic operand");
        }

        return code_[pos_];
    }

    uint16_t u16() {
        uint16_t low = byte();
        return static_cast<uint16_t>(low | (byte() << 8));
    }

    void zero() {
        if (byte() != 0) {
            throw std::runtime_error("gm: expected semantic terminator");
        }
    }

    AstNode bytes(size_t count, const std::string& tag = "bytes") {
        std::vector<AstNode> values;
        values.reserve(count);

        for (size_t i = 0; i < count; i++) {
            values.push_back(integer(byte()));
        }

        return list(tag, std::move(values));
    }

    AstNode reference() {
        uint8_t token = byte();

        if (token < 5 || token > 0x12) {
            throw std::runtime_error("gm: invalid semantic reference token");
        }

        std::vector<AstNode> children = {integer(token), integer(u16())};

        if (token <= 0x0a) {
            children.push_back(expression());
        }

        return list("ref", std::move(children));
    }

    struct Operand {
        AstNode node;
        size_t literal_width = 0;
    };

    Operand operand(bool initial) {
        uint8_t token = peek();

        if (initial && token == 0x32) {
            byte();
            return {list("random", {integer(u16()), integer(u16())})};
        }

        if (token >= 1 && token <= 4) {
            size_t width = byte();
            uint32_t value = 0;

            for (size_t i = 0; i < width; i++) {
                value |= static_cast<uint32_t>(byte()) << (i * 8);
            }

            return {integer(value), width};
        }

        return {reference()};
    }

    AstNode expression(size_t* literal_width = nullptr) {
        std::vector<AstNode> terms;

        if (peek() == 0) {
            byte();
            if (literal_width != nullptr) *literal_width = 0;
            return AstNode::make_symbol("default");
        }

        auto first = operand(true);
        size_t pure_literal_width = first.literal_width;
        terms.push_back(std::move(first.node));

        while (peek() != 0) {
            uint8_t token = peek();

            if (token >= 0x20) {
                pure_literal_width = 0;
                byte();
                const char* name = operator_name(token);

                if (name == nullptr) {
                    throw std::runtime_error("gm: unsupported expression operator");
                }
                terms.push_back(AstNode::make_symbol(name));
            } else {
                pure_literal_width = 0;
                terms.push_back(std::move(operand(false).node));
            }
        }

        zero();
        if (literal_width != nullptr) *literal_width = pure_literal_width;

        // GM stores expressions as postfix streams. Present balanced streams
        // as the same nested operator trees used by the older Juice engines,
        // but retain the native flat stream for malformed expressions found
        // in shipped data (for example a trailing operator).
        std::vector<AstNode> stack;

        for (const auto& term : terms) {
            if (!term.is_symbol()) {
                stack.push_back(term);
                continue;
            }

            if (stack.size() < 2) {
                return list("postfix", std::move(terms));
            }

            AstNode right = std::move(stack.back());
            stack.pop_back();
            AstNode left = std::move(stack.back());
            stack.pop_back();
            stack.push_back(list(term.str_val,
                                 {std::move(left), std::move(right)}));
        }

        if (stack.size() == 1) {
            return std::move(stack.back());
        }

        return list("postfix", std::move(terms));
    }

    std::vector<AstNode> params() {
        std::vector<AstNode> values;

        while (peek() != 0) {
            if (peek() == 0x11) {
                byte();
                std::vector<uint8_t> string_bytes;

                while (peek() != 0) {
                    string_bytes.push_back(byte());
                }

                zero();
                bool printable = decode_strings_;
                for (uint8_t value : string_bytes) {
                    printable = printable && value >= 0x20 && value <= 0x7e;
                }

                if (printable) {
                    values.push_back(AstNode::make_string(std::string(
                        string_bytes.begin(), string_bytes.end())));
                } else {
                    std::vector<AstNode> raw;
                    raw.reserve(string_bytes.size());
                    for (uint8_t value : string_bytes) raw.push_back(integer(value));
                    values.push_back(list("string-bytes", std::move(raw)));
                }
            } else if (peek() == 0x0f) {
                values.push_back(list("ref-param", {reference()}));
                zero();
            } else {
                values.push_back(expression());
            }
        }

        zero();
        return values;
    }

    std::vector<AstNode> reference_list() {
        std::vector<AstNode> values;

        while (peek() != 0) {
            values.push_back(reference());
            zero();
        }

        zero();
        return values;
    }

    AstNode address() {
        size_t field = base_ + pos_;
        uint16_t target = u16();
        return list(local_fields_.count(field) != 0
                        ? "local-address" : "address",
                    {integer(target)});
    }

    std::string command_name() const {
        if (!resolve_) {
            return "cmd:" + std::to_string(opcode_);
        }

        const char* name = opcode_name(opcode_);
        if (name == nullptr) {
            throw std::runtime_error("gm: unsupported semantic opcode");
        }
        return name;
    }

    AstNode menu() {
        uint8_t subtype = byte();
        std::vector<AstNode> children = {integer(subtype)};

        if (subtype == 1) {
            auto selector = expression();
            uint32_t selector_value = 0;
            children.push_back(selector);
            children.push_back(expression());

            if (literal_value(selector, selector_value) && selector_value == 1) {
                for (size_t i = 0; i < 6; i++) {
                    size_t width = 0;
                    auto value = expression(&width);
                    uint32_t ignored = 0;

                    if (i == 4 && literal_value(value, ignored) && width == 2) {
                        value = list("callback", {
                            list("local-address", {integer(ignored)})
                        });
                    }

                    children.push_back(std::move(value));
                }
            }
        } else if (subtype == 2 || subtype == 3 || subtype == 6) {
            children.push_back(reference());
            zero();
        } else if (subtype == 4) {
            for (size_t i = 0; i < 9; i++) {
                children.push_back(expression());
            }
        } else if (subtype == 5) {
            for (size_t i = 0; i < 3; i++) {
                children.push_back(expression());
            }

            for (size_t i = 0; i < 2; i++) {
                children.push_back(reference());
                zero();
            }
        } else if (subtype == 7 || subtype == 8 || subtype == 9) {
            children.push_back(expression());
        } else if (subtype == 10) {
            auto selector = expression();
            uint32_t selector_value = 0;
            bool literal = literal_value(selector, selector_value);
            children.push_back(selector);
            size_t count = literal && selector_value == 0 ? 7 : 6;

            for (size_t i = 0; i < count; i++) {
                children.push_back(reference());
                zero();
            }

            if (!literal && peek() != 0) {
                children.push_back(reference());
                zero();
            }
        } else if (subtype != 11 && subtype != 12) {
            throw std::runtime_error("gm: unknown menu subtype");
        }

        zero();
        return list(command_name(), std::move(children));
    }

    AstNode assignment() {
        std::vector<AstNode> children = {reference()};

        while (true) {
            AstNode value;

            if (peek() == 0x0e) {
                value = list("string-value", {reference()});
                zero();
            } else {
                value = expression();
            }

            std::vector<AstNode> strides;

            while (peek() == 0x31) {
                byte();
                strides.push_back(integer(byte()));
            }

            if (!strides.empty()) {
                children.push_back(list("value", {
                    std::move(value), list("strides", std::move(strides))
                }));
            } else {
                children.push_back(std::move(value));
            }

            if (peek() == 0) {
                zero();
                break;
            }

            if (peek() == 0x30) {
                byte();
                children.push_back(list("range", {reference()}));
                zero();
                break;
            }
        }

        return list(command_name(), std::move(children));
    }

    AstNode struct_assignment() {
        AstNode destination = reference();
        uint16_t word = u16();

        if (peek() == 0x0f) {
            AstNode source = reference();
            zero();
            return list(command_name(), {
                std::move(destination), integer(word),
                list("ref-source", {std::move(source)})
            });
        }

        std::vector<uint8_t> payload;
        size_t target = word;
        size_t absolute_pos = base_ + pos_;

        if (target <= absolute_pos || target != base_ + end_) {
            throw std::runtime_error("gm: inconsistent inline struct target");
        }

        while (pos_ < end_) {
            payload.push_back(byte());
        }

        bool printable_string = payload.size() >= 2 && payload.front() == 0x11 &&
                                payload.back() == 0;
        for (size_t i = 1; printable_string && i + 1 < payload.size(); i++) {
            printable_string = payload[i] >= 0x20 && payload[i] <= 0x7e;
        }

        if (printable_string) {
            return list(command_name(), {
                std::move(destination), AstNode::make_string(std::string(
                    payload.begin() + 1, payload.end() - 1))
            });
        }

        std::vector<AstNode> raw;
        raw.reserve(payload.size());
        for (uint8_t value : payload) raw.push_back(integer(value));
        return list(command_name(), {
            std::move(destination), list("inline", std::move(raw))
        });
    }

    AstNode string_copy() {
        AstNode destination = reference();
        uint8_t first = peek();

        if (pos_ + 1 >= end_) {
            throw std::runtime_error("gm: truncated string source");
        }

        if (code_[pos_ + 1] >= 5) {
            byte();
            AstNode source = reference();
            zero();
            return list(command_name(), {
                std::move(destination),
                list("ref-source", {integer(first), std::move(source)})
            });
        }

        std::vector<AstNode> source;

        while (peek() != 0) {
            source.push_back(integer(byte()));
        }

        zero();
        uint8_t trailing = byte();
        std::vector<AstNode> children = {integer(trailing)};
        children.insert(children.end(), std::make_move_iterator(source.begin()),
                        std::make_move_iterator(source.end()));
        return list(command_name(), {
            std::move(destination), list("inline-source", std::move(children))
        });
    }

    AstNode decode_body() {
        std::string name = command_name();

        if (opcode_ == 0x4a) {
            throw std::runtime_error("gm: unsupported semantic opcode");
        }

        if (opcode_ == 0) {
            return list(name);
        }

        switch (opcode_) {
            case 0x30: case 0x36: case 0x38: case 0x3d: case 0x41:
            case 0x42: case 0x50: case 0x56: case 0x57: case 0x66:
            case 0x69: case 0x70: case 0x7c: case 0x7f: case 0x80:
                return list(name);

            case 0x31: case 0x83:
                return list(name, {integer(u16()), address(), expression()});
            case 0x32:
                return list(name, {integer(u16()), address()});
            case 0x33: {
                AstNode target = address();
                AstNode condition = expression();
                return list(name, {std::move(condition), std::move(target)});
            }
            case 0x34:
                return list(name, {address(), expression()});
            case 0x35: {
                std::vector<AstNode> children = {address()};
                while (peek() != 0) children.push_back(expression());
                zero();
                return list(name, std::move(children));
            }
            case 0x37: {
                AstNode value = expression();
                if (value.is_symbol("default")) return list(name);
                return list(name, {std::move(value)});
            }
            case 0x39: case 0x3f: case 0x40:
                return list(name, {address(), address(), expression()});
            case 0x3a:
                return menu();
            case 0x3b: {
                AstNode value = expression();
                zero();
                return list(name, {std::move(value)});
            }
            case 0x3c:
                return list(name, {bytes(5)});
            case 0x3e: {
                uint8_t mode = byte();
                zero();
                return list(name, {integer(mode)});
            }
            case 0x43:
                return assignment();
            case 0x44:
                return struct_assignment();
            case 0x45:
                return string_copy();
            case 0x4b: {
                AstNode ref = reference();
                uint8_t first = byte();
                uint8_t second = byte();
                if (first == 0 && second == 0) {
                    return list(name, {std::move(ref)});
                }
                return list(name, {std::move(ref), integer(first), integer(second)});
            }
            case 0x46: case 0x47: case 0x48: case 0x49: case 0x4c:
            case 0x4d: case 0x4e: case 0x4f: case 0x51: case 0x52:
            case 0x53: case 0x54: case 0x55: case 0x58: case 0x59:
            case 0x5a: case 0x5b: case 0x5c: case 0x5d: case 0x5e:
            case 0x5f: case 0x60: case 0x61: case 0x63: case 0x67:
            case 0x6c: case 0x6d: case 0x6e: case 0x6f: case 0x74:
            case 0x75: case 0x78: case 0x7a: case 0x7d: case 0x81:
            case 0x82:
                return list(name, params());
            case 0x62:
                zero();
                return list(name);
            case 0x64: {
                AstNode ref = reference();
                zero();
                zero();
                return list(name, {std::move(ref)});
            }
            case 0x65: case 0x68: case 0x6a:
                return list(name, reference_list());
            case 0x6b: {
                AstNode selector = expression();
                uint32_t value = 0;
                if (!literal_value(selector, value)) {
                    throw std::runtime_error("gm: non-literal mouse selector");
                }
                std::vector<AstNode> children = {std::move(selector)};
                if (value == 0 || value == 1 || value == 7 || value == 8) {
                    zero();
                } else if (value == 2 || value == 3) {
                    auto refs = reference_list();
                    children.insert(children.end(),
                                    std::make_move_iterator(refs.begin()),
                                    std::make_move_iterator(refs.end()));
                } else {
                    auto values = params();
                    children.insert(children.end(),
                                    std::make_move_iterator(values.begin()),
                                    std::make_move_iterator(values.end()));
                }
                return list(name, std::move(children));
            }
            case 0x71: {
                AstNode selector = expression();
                uint32_t value = 0;
                if (!literal_value(selector, value)) {
                    throw std::runtime_error("gm: non-literal video selector");
                }
                std::vector<AstNode> children = {std::move(selector)};
                if (value == 0) {
                    children.push_back(reference());
                    zero();
                    zero();
                } else if (value == 1) {
                    children.push_back(expression());
                    zero();
                } else {
                    auto values = params();
                    children.insert(children.end(),
                                    std::make_move_iterator(values.begin()),
                                    std::make_move_iterator(values.end()));
                }
                return list(name, std::move(children));
            }
            case 0x72: case 0x73: case 0x7e: {
                AstNode first = expression();
                AstNode ref = reference();
                zero();
                AstNode last = expression();
                zero();
                return list(name, {std::move(first), std::move(ref), std::move(last)});
            }
            case 0x76: case 0x77: {
                AstNode first = expression();
                AstNode second = expression();
                zero();
                return list(name, {std::move(first), std::move(second)});
            }
            case 0x79: {
                std::vector<AstNode> children = {expression()};
                auto refs = reference_list();
                children.insert(children.end(), std::make_move_iterator(refs.begin()),
                                std::make_move_iterator(refs.end()));
                return list(name, std::move(children));
            }
            case 0x7b: {
                std::vector<AstNode> children = {reference()};
                zero();
                auto values = params();
                children.insert(children.end(), std::make_move_iterator(values.begin()),
                                std::make_move_iterator(values.end()));
                return list(name, std::move(children));
            }
            case 0x84: case 0x85: {
                AstNode ref = reference();
                uint8_t first = byte();
                uint8_t second = byte();
                return list(name, {std::move(ref), integer(first), integer(second)});
            }
            default:
                throw std::runtime_error("gm: missing semantic opcode layout");
        }
    }

    const std::vector<uint8_t>& code_;
    size_t base_;
    size_t end_;
    size_t pos_;
    uint8_t opcode_;
    const std::unordered_set<size_t>& local_fields_;
    bool resolve_;
    bool decode_strings_;
};

int checked_integer(const AstNode& node, const std::string& what,
                    int minimum, int maximum) {
    if (!node.is_integer() || node.int_val < minimum || node.int_val > maximum) {
        throw std::runtime_error("gm: invalid " + what);
    }

    return node.int_val;
}

void expect_children(const AstNode& node, size_t count) {
    if (node.children.size() != count) {
        throw std::runtime_error("gm: malformed " + node.tag);
    }
}

void emit_bytes(ByteWriter& out, const AstNode& node, const std::string& tag) {
    if (!node.is_list(tag)) {
        throw std::runtime_error("gm: expected " + tag);
    }

    for (const auto& value : node.children) {
        out.emit(static_cast<uint8_t>(checked_integer(value, tag + " byte", 0, 255)));
    }
}

void emit_expression(ByteWriter& out, const AstNode& expression);

void emit_reference(ByteWriter& out, const AstNode& reference) {
    if (!reference.is_list("ref") ||
        (reference.children.size() != 2 && reference.children.size() != 3)) {
        throw std::runtime_error("gm: expected (ref TOKEN OFFSET [INDEX])");
    }

    int token = checked_integer(reference.children[0], "reference token", 5, 0x12);
    int offset = checked_integer(reference.children[1], "reference offset", 0, 65535);
    bool indexed = token <= 0x0a;

    if (reference.children.size() != (indexed ? 3u : 2u)) {
        throw std::runtime_error("gm: indexed reference shape does not match token");
    }

    out.emit(static_cast<uint8_t>(token));
    out.emit_u16_le(static_cast<uint16_t>(offset));

    if (indexed) {
        emit_expression(out, reference.children[2]);
    }
}

uint8_t operator_token(const AstNode& node) {
    std::string name;
    if (node.is_symbol()) {
        name = node.str_val;
    } else if (node.is_list()) {
        name = node.tag;
    } else {
        throw std::runtime_error("gm: expected expression term or operator");
    }

    static const std::unordered_map<std::string, uint8_t> operators = {
        {"*", 0x20}, {"/", 0x21}, {"%", 0x22}, {"+", 0x23},
        {"-", 0x24}, {"&", 0x25}, {"|", 0x26}, {"==", 0x28},
        {"!=", 0x29}, {"<", 0x2a}, {"<=", 0x2b}, {">", 0x2c},
        {">=", 0x2d}, {"&&", 0x2e}, {"||", 0x2f}
    };
    auto found = operators.find(name);

    if (found == operators.end()) {
        throw std::runtime_error("gm: unknown expression operator: " + name);
    }

    return found->second;
}

void emit_expression_term(ByteWriter& out, const AstNode& term,
                          bool& emitted_any) {
    if (term.is_integer()) {
        uint32_t value = static_cast<uint32_t>(term.int_val);
        int width = value <= 0xff ? 1 : value <= 0xffff ? 2 :
                    value <= 0xffffff ? 3 : 4;
        out.emit(static_cast<uint8_t>(width));
        for (int byte_index = 0; byte_index < width; byte_index++) {
            out.emit(static_cast<uint8_t>(value >> (byte_index * 8)));
        }
    } else if (term.is_list("ref")) {
        emit_reference(out, term);
    } else if (term.is_list("random")) {
        if (emitted_any) {
            throw std::runtime_error("gm: random range must be the first expression term");
        }
        expect_children(term, 2);
        out.emit(0x32);
        out.emit_u16_le(static_cast<uint16_t>(checked_integer(
            term.children[0], "random lower bound", 0, 65535)));
        out.emit_u16_le(static_cast<uint16_t>(checked_integer(
            term.children[1], "random upper bound", 0, 65535)));
    } else if (term.is_list()) {
        expect_children(term, 2);
        emit_expression_term(out, term.children[0], emitted_any);
        emit_expression_term(out, term.children[1], emitted_any);
        out.emit(operator_token(term));
    } else {
        out.emit(operator_token(term));
    }

    emitted_any = true;
}

void emit_expression(ByteWriter& out, const AstNode& expression) {
    if (expression.is_symbol("default")) {
        out.emit(0);
        return;
    }

    bool emitted_any = false;
    if (expression.is_list("postfix")) {
        for (const auto& term : expression.children) {
            emit_expression_term(out, term, emitted_any);
        }
    } else {
        emit_expression_term(out, expression, emitted_any);
    }

    out.emit(0);
}

void emit_params(ByteWriter& out, const std::vector<AstNode>& values,
                 size_t first = 0) {
    for (size_t i = first; i < values.size(); i++) {
        const auto& value = values[i];

        if (value.is_string()) {
            out.emit(0x11);
            for (unsigned char byte : value.str_val) {
                if (byte < 0x20 || byte > 0x7e) {
                    throw std::runtime_error(
                        "gm: string parameters only support printable ASCII; "
                        "use string-bytes for other data");
                }
                out.emit(byte);
            }
            out.emit(0);
        } else if (value.is_list("string-bytes")) {
            out.emit(0x11);
            for (const auto& byte : value.children) {
                out.emit(static_cast<uint8_t>(checked_integer(
                    byte, "string-bytes byte", 1, 255)));
            }
            out.emit(0);
        } else if (value.is_list("ref-param")) {
            expect_children(value, 1);
            if (!value.children[0].is_list("ref") ||
                value.children[0].children.empty() ||
                !value.children[0].children[0].is_integer() ||
                value.children[0].children[0].int_val != 0x0f) {
                throw std::runtime_error("gm: reference parameter must use token 0x0f");
            }
            emit_reference(out, value.children[0]);
            out.emit(0);
        } else {
            emit_expression(out, value);
        }
    }

    out.emit(0);
}

void emit_reference_list(ByteWriter& out, const std::vector<AstNode>& values,
                         size_t first = 0) {
    for (size_t i = first; i < values.size(); i++) {
        emit_reference(out, values[i]);
        out.emit(0);
    }

    out.emit(0);
}

uint32_t require_literal(const AstNode& expression, const std::string& what) {
    uint32_t value = 0;

    if (!literal_value(expression, value)) {
        throw std::runtime_error("gm: " + what + " must be a literal expression");
    }

    return value;
}

void emit_address(ByteWriter& out, const AstNode& address,
                  std::vector<SemanticRelocation>& relocations) {
    bool local = address.is_list("local-address");
    if (!local && !address.is_list("address")) {
        throw std::runtime_error("gm: expected address or local-address");
    }

    expect_children(address, 1);
    uint16_t target = static_cast<uint16_t>(checked_integer(
        address.children[0], "control address", 0, 65535));
    size_t field = out.size();
    out.emit_u16_le(target);
    relocations.push_back({field, target, local});
}

void emit_menu(ByteWriter& out, const AstNode& node,
               std::vector<SemanticRelocation>& relocations) {
    if (node.children.empty()) {
        throw std::runtime_error("gm: menu requires a subtype");
    }

    int subtype = checked_integer(node.children[0], "menu subtype", 1, 12);
    uint32_t selector = 0;
    bool literal_selector = node.children.size() > 1 &&
                            literal_value(node.children[1], selector);
    size_t expected = 0;

    if (subtype == 1) {
        expected = literal_selector && selector == 1 ? 9 : 3;
    } else if (subtype == 2 || subtype == 3 || subtype == 6 ||
               subtype == 7 || subtype == 8 || subtype == 9) {
        expected = 2;
    } else if (subtype == 4) {
        expected = 10;
    } else if (subtype == 5) {
        expected = 6;
    } else if (subtype == 10) {
        if (!literal_selector) {
            if (node.children.size() != 8 && node.children.size() != 9) {
                throw std::runtime_error("gm: non-literal menu subtype 10 needs 6 or 7 references");
            }
        } else {
            expected = selector == 0 ? 9 : 8;
        }
    } else {
        expected = 1;
    }

    if (expected != 0 && node.children.size() != expected) {
        throw std::runtime_error("gm: wrong argument count for menu subtype " +
                                 std::to_string(subtype));
    }

    out.emit(static_cast<uint8_t>(subtype));

    for (size_t i = 1; i < node.children.size(); i++) {
        const auto& value = node.children[i];

        if (value.is_list("ref")) {
            emit_reference(out, value);
            out.emit(0);
        } else if (value.is_list("callback")) {
            expect_children(value, 1);
            if (!value.children[0].is_list("local-address")) {
                throw std::runtime_error(
                    "gm: menu callback must use local-address");
            }
            out.emit(2);
            emit_address(out, value.children[0], relocations);
            out.emit(0);
        } else {
            emit_expression(out, value);
        }
    }

    out.emit(0);
}

void emit_assignment(ByteWriter& out, const AstNode& node) {
    if (node.children.size() < 2) {
        throw std::runtime_error("gm: assignment requires a destination and value");
    }

    emit_reference(out, node.children[0]);
    bool range_seen = false;

    for (size_t i = 1; i < node.children.size(); i++) {
        const auto& child = node.children[i];

        if (child.is_list("range")) {
            if (range_seen || i + 1 != node.children.size()) {
                throw std::runtime_error("gm: assignment range must be last");
            }
            expect_children(child, 1);
            out.emit(0x30);
            emit_reference(out, child.children[0]);
            out.emit(0);
            range_seen = true;
            continue;
        }

        const AstNode* value = &child;
        const AstNode* strides = nullptr;
        if (child.is_list("value")) {
            if (child.children.size() != 2 ||
                !child.children[1].is_list("strides")) {
                throw std::runtime_error("gm: malformed assignment value");
            }
            value = &child.children[0];
            strides = &child.children[1];
        }

        if (value->is_list("string-value")) {
            expect_children(*value, 1);
            if (!value->children[0].is_list("ref") ||
                value->children[0].children.empty() ||
                !value->children[0].children[0].is_integer() ||
                value->children[0].children[0].int_val != 0x0e) {
                throw std::runtime_error("gm: string assignment must use reference token 0x0e");
            }
            emit_reference(out, value->children[0]);
            out.emit(0);
        } else {
            emit_expression(out, *value);
        }

        if (strides != nullptr) {
            for (const auto& stride : strides->children) {
                out.emit(0x31);
                out.emit(static_cast<uint8_t>(checked_integer(stride, "stride", 0, 255)));
            }
        }
    }

    if (!range_seen) {
        out.emit(0);
    }
}

void emit_struct_assignment(ByteWriter& out, const AstNode& node) {
    if (node.children.size() == 3 && node.children[1].is_integer() &&
        node.children[2].is_list("ref-source")) {
        emit_reference(out, node.children[0]);
        out.emit_u16_le(static_cast<uint16_t>(checked_integer(
            node.children[1], "struct word", 0, 65535)));
        expect_children(node.children[2], 1);
        if (!node.children[2].children[0].is_list("ref") ||
            node.children[2].children[0].children.empty() ||
            !node.children[2].children[0].children[0].is_integer() ||
            node.children[2].children[0].children[0].int_val != 0x0f) {
            throw std::runtime_error("gm: struct reference source must use token 0x0f");
        }
        emit_reference(out, node.children[2].children[0]);
        out.emit(0);
        return;
    }

    if (node.children.size() == 2 && node.children[1].is_string()) {
        emit_reference(out, node.children[0]);
        size_t field = out.size();
        out.emit_u16_le(0);
        out.emit(0x11);
        for (unsigned char byte : node.children[1].str_val) {
            if (byte < 0x20 || byte > 0x7e) {
                throw std::runtime_error(
                    "gm: inline struct strings must be printable ASCII");
            }
            out.emit(byte);
        }
        out.emit(0);

        if (out.size() > 65535) {
            throw std::runtime_error("gm: inline struct target exceeds 16 bits");
        }

        out.write_u16_le_at(field, static_cast<uint16_t>(out.size()));
        return;
    }

    if (node.children.size() == 2 && node.children[1].is_list("inline")) {
        const auto& payload = node.children[1].children;
        if (payload.empty()) {
            throw std::runtime_error("gm: inline struct payload must not be empty");
        }
        if (checked_integer(payload.front(), "inline byte", 0, 255) == 0x0f) {
            throw std::runtime_error(
                "gm: inline struct payload would decode as a reference source");
        }
        emit_reference(out, node.children[0]);
        size_t field = out.size();
        out.emit_u16_le(0);
        emit_bytes(out, node.children[1], "inline");

        if (out.size() > 65535) {
            throw std::runtime_error("gm: inline struct target exceeds 16 bits");
        }

        out.write_u16_le_at(field, static_cast<uint16_t>(out.size()));
        return;
    }

    throw std::runtime_error("gm: malformed struct assignment");
}

void emit_string_copy(ByteWriter& out, const AstNode& node) {
    expect_children(node, 2);
    emit_reference(out, node.children[0]);
    const auto& source = node.children[1];

    if (source.is_list("ref-source")) {
        expect_children(source, 2);
        out.emit(static_cast<uint8_t>(checked_integer(source.children[0],
                                                      "string source prefix", 0, 255)));
        emit_reference(out, source.children[1]);
        out.emit(0);
    } else if (source.is_list("inline-source") && !source.children.empty()) {
        uint8_t trailing = static_cast<uint8_t>(checked_integer(
            source.children[0], "string trailing byte", 0, 255));
        std::vector<uint8_t> payload;
        payload.reserve(source.children.size() - 1);
        for (size_t i = 1; i < source.children.size(); i++) {
            payload.push_back(static_cast<uint8_t>(checked_integer(
                source.children[i], "inline string byte", 1, 255)));
        }

        uint8_t discriminator = payload.size() >= 2
            ? payload[1]
            : payload.empty() ? trailing : 0;
        if (discriminator >= 5) {
            throw std::runtime_error(
                "gm: inline string source would decode as a reference source");
        }

        for (uint8_t byte : payload) out.emit(byte);
        out.emit(0);
        out.emit(trailing);
    } else {
        throw std::runtime_error("gm: malformed string source");
    }
}

uint8_t opcode_for_tag(const std::string& tag) {
    auto prefixed_opcode = [&](const std::string& prefix) -> int {
        if (tag.rfind(prefix, 0) != 0 || tag.size() == prefix.size()) {
            return -1;
        }

        int value = 0;
        for (size_t i = prefix.size(); i < tag.size(); i++) {
            char ch = tag[i];
            if (ch < '0' || ch > '9') return -1;
            value = value * 10 + (ch - '0');
            if (value > 255) return -1;
        }
        return value;
    };

    int command = prefixed_opcode("cmd:");
    if (command == 0 || (command >= 0x30 && command <= 0x85)) {
        return static_cast<uint8_t>(command);
    }

    int nop = prefixed_opcode("nop:");
    if (nop == 0x30 || nop == 0x36 || nop == 0x42 || nop == 0x7f) {
        return static_cast<uint8_t>(nop);
    }

    for (int opcode = 0; opcode <= 0x85; opcode++) {
        const char* name = opcode_name(static_cast<uint8_t>(opcode));
        if (name != nullptr && tag == name) return static_cast<uint8_t>(opcode);
    }
    return 0xff;
}

} // namespace

const char* opcode_name(uint8_t opcode) {
    switch (opcode) {
        case 0x00: return "end";
        case 0x30: return "nop:48";
        case 0x31: return "for-start";
        case 0x32: return "for-continue";
        case 0x33: return "if-frame";
        case 0x34: return "switch";
        case 0x35: return "case";
        case 0x36: return "nop:54";
        case 0x37: return "next";
        case 0x38: return "while-continue";
        case 0x39: return "gosub-if";
        case 0x3a: return "menu";
        case 0x3b: return "eval";
        case 0x3c: return "skip-5";
        case 0x3d: return "menu-click-wait";
        case 0x3e: return "cursor";
        case 0x3f: return "call-reset-if";
        case 0x40: return "gosub-if-save";
        case 0x41: return "return";
        case 0x42: return "nop:66";
        case 0x43: return "assign";
        case 0x44: return "struct-assign";
        case 0x45: return "string-copy";
        case 0x46: return "text-window";
        case 0x47: return "text-origin";
        case 0x48: return "text-window-stack";
        case 0x49: return "text-attribute";
        case 0x4a: return "text";
        case 0x4b: return "text-indirect";
        case 0x4c: return "number";
        case 0x4d: return "text-color";
        case 0x4e: return "palette-color-map";
        case 0x4f: return "text-clear";
        case 0x50: return "message-end";
        case 0x51: return "palette-set";
        case 0x52: return "palette-fill";
        case 0x53: return "palette-work-fill";
        case 0x54: return "palette-apply";
        case 0x55: return "fade-wait";
        case 0x56: return "palette-save";
        case 0x57: return "palette-restore";
        case 0x58: return "display-page";
        case 0x59: return "display-plane";
        case 0x5a: return "image-open";
        case 0x5b: return "fill-rect";
        case 0x5c: return "box-rect";
        case 0x5d: return "blit";
        case 0x5e: return "blit-variant";
        case 0x5f: return "blit-mode";
        case 0x60: return "window-blit-setup";
        case 0x61: return "window-blit-setup-alt";
        case 0x62: return "gdc-window-init";
        case 0x63: return "sprite";
        case 0x64: return "input";
        case 0x65: return "store-values";
        case 0x66: return "wait-key";
        case 0x67: return "delay";
        case 0x68: return "store-six-values";
        case 0x69: return "tick-snapshot";
        case 0x6a: return "tick-delta";
        case 0x6b: return "mouse-command";
        case 0x6c: return "drive-slot";
        case 0x6d: return "mes-jump";
        case 0x6e: return "mll-load";
        case 0x6f: return "mes-call";
        case 0x70: return "loop-end";
        case 0x71: return "video-command";
        case 0x72: return "file-save-range";
        case 0x73: return "file-load-range";
        case 0x74: return "save-slot";
        case 0x75: return "load-slot";
        case 0x76: return "image-load";
        case 0x77: return "image-load-alt";
        case 0x78: return "vram-bank";
        case 0x79: return "file-date";
        case 0x7a: return "music-command";
        case 0x7b: return "hit-test";
        case 0x7c: return "hook";
        case 0x7d: return "driver-state";
        case 0x7e: return "progress-merge";
        case 0x7f: return "nop:127";
        case 0x80: return "beyond-flag-test";
        case 0x81: return "beyond-external-call";
        case 0x82: return "beyond-bank";
        case 0x83: return "for-end";
        case 0x84: return "push-reference";
        case 0x85: return "pop-reference";
        default: return nullptr;
    }
}

AstNode decode_instruction(const std::vector<uint8_t>& code, size_t code_base,
                           const InstructionSpan& instruction,
                           const std::unordered_set<size_t>& local_fields,
                           bool resolve, bool decode_strings) {
    try {
        return Decoder(code, code_base, instruction, local_fields, resolve,
                       decode_strings).decode();
    } catch (const std::exception& error) {
        throw std::runtime_error(
            "gm: semantic opcode " + std::to_string(instruction.opcode) +
            " at " + std::to_string(instruction.start) + ": " + error.what());
    }
}

bool emit_instruction(ByteWriter& out, const AstNode& node,
                      std::vector<SemanticRelocation>& relocations) {
    if (node.is_list("call")) {
        if (node.children.empty() || node.children.size() > 2) {
            throw std::runtime_error("gm: expected (call ADDRESS [CONDITION])");
        }

        out.emit(0x40);
        size_t continuation_field = out.size();
        out.emit_u16_le(0);
        emit_address(out, node.children[0], relocations);
        if (node.children.size() == 2) {
            emit_expression(out, node.children[1]);
        } else {
            out.emit(0);
        }
        out.emit(0x00);

        if (out.size() > 0xffff) {
            throw std::runtime_error("gm: call continuation exceeds 16 bits");
        }
        out.write_u16_le_at(continuation_field,
                            static_cast<uint16_t>(out.size()));
        return true;
    }

    uint8_t opcode = opcode_for_tag(node.tag);

    if (opcode == 0xff || opcode == 0x4a) {
        return false;
    }

    out.emit(opcode);

    switch (opcode) {
        case 0x00: case 0x30: case 0x36: case 0x38: case 0x3d:
        case 0x41: case 0x42: case 0x50: case 0x56: case 0x57:
        case 0x66: case 0x69: case 0x70: case 0x7c: case 0x7f:
        case 0x80:
            expect_children(node, 0);
            break;
        case 0x31: case 0x83:
            expect_children(node, 3);
            out.emit_u16_le(static_cast<uint16_t>(checked_integer(
                node.children[0], "loop id", 0, 65535)));
            emit_address(out, node.children[1], relocations);
            emit_expression(out, node.children[2]);
            break;
        case 0x32:
            expect_children(node, 2);
            out.emit_u16_le(static_cast<uint16_t>(checked_integer(
                node.children[0], "loop id", 0, 65535)));
            emit_address(out, node.children[1], relocations);
            break;
        case 0x33:
            expect_children(node, 2);
            emit_address(out, node.children[1], relocations);
            emit_expression(out, node.children[0]);
            break;
        case 0x34:
            expect_children(node, 2);
            emit_address(out, node.children[0], relocations);
            emit_expression(out, node.children[1]);
            break;
        case 0x35:
            if (node.children.empty()) throw std::runtime_error("gm: case requires address");
            emit_address(out, node.children[0], relocations);
            for (size_t i = 1; i < node.children.size(); i++) emit_expression(out, node.children[i]);
            out.emit(0);
            break;
        case 0x37:
            if (node.children.size() > 1) {
                throw std::runtime_error("gm: next takes at most one expression");
            }
            if (node.children.empty()) out.emit(0);
            else emit_expression(out, node.children[0]);
            break;
        case 0x39: case 0x3f: case 0x40:
            expect_children(node, 3);
            emit_address(out, node.children[0], relocations);
            emit_address(out, node.children[1], relocations);
            emit_expression(out, node.children[2]);
            break;
        case 0x3a:
            emit_menu(out, node, relocations);
            break;
        case 0x3b:
            expect_children(node, 1);
            emit_expression(out, node.children[0]);
            out.emit(0);
            break;
        case 0x3c:
            expect_children(node, 1);
            if (!node.children[0].is_list("bytes") || node.children[0].children.size() != 5)
                throw std::runtime_error("gm: skip-5 requires five bytes");
            emit_bytes(out, node.children[0], "bytes");
            break;
        case 0x3e:
            expect_children(node, 1);
            out.emit(static_cast<uint8_t>(checked_integer(node.children[0], "cursor mode", 1, 2)));
            out.emit(0);
            break;
        case 0x43:
            emit_assignment(out, node);
            break;
        case 0x44:
            emit_struct_assignment(out, node);
            break;
        case 0x45:
            emit_string_copy(out, node);
            break;
        case 0x4b:
            if (node.children.size() != 1 && node.children.size() != 3) {
                throw std::runtime_error(
                    "gm: text-indirect takes a reference and optional trailing bytes");
            }
            emit_reference(out, node.children[0]);
            if (node.children.size() == 1) {
                out.emit(0);
                out.emit(0);
            } else {
                out.emit(static_cast<uint8_t>(checked_integer(node.children[1], "trailing byte", 0, 255)));
                out.emit(static_cast<uint8_t>(checked_integer(node.children[2], "trailing byte", 0, 255)));
            }
            break;
        case 0x84: case 0x85:
            expect_children(node, 3);
            emit_reference(out, node.children[0]);
            out.emit(static_cast<uint8_t>(checked_integer(node.children[1], "trailing byte", 0, 255)));
            out.emit(static_cast<uint8_t>(checked_integer(node.children[2], "trailing byte", 0, 255)));
            break;
        case 0x46: case 0x47: case 0x48: case 0x49: case 0x4c:
        case 0x4d: case 0x4e: case 0x4f: case 0x51: case 0x52:
        case 0x53: case 0x54: case 0x55: case 0x58: case 0x59:
        case 0x5a: case 0x5b: case 0x5c: case 0x5d: case 0x5e:
        case 0x5f: case 0x60: case 0x61: case 0x63: case 0x67:
        case 0x6c: case 0x6d: case 0x6e: case 0x6f: case 0x74:
        case 0x75: case 0x78: case 0x7a: case 0x7d: case 0x81:
        case 0x82:
            emit_params(out, node.children);
            break;
        case 0x62:
            expect_children(node, 0);
            out.emit(0);
            break;
        case 0x64:
            expect_children(node, 1);
            emit_reference(out, node.children[0]);
            out.emit(0);
            out.emit(0);
            break;
        case 0x65: case 0x68: case 0x6a:
            emit_reference_list(out, node.children);
            break;
        case 0x6b: {
            if (node.children.empty()) throw std::runtime_error("gm: mouse command requires selector");
            emit_expression(out, node.children[0]);
            uint32_t selector = require_literal(node.children[0], "mouse selector");
            if (selector > 9) throw std::runtime_error("gm: mouse selector must be from 0 through 9");
            if (selector == 0 || selector == 1 || selector == 7 || selector == 8) {
                if (node.children.size() != 1) throw std::runtime_error("gm: mouse selector takes no arguments");
                out.emit(0);
            } else if (selector == 2 || selector == 3) {
                emit_reference_list(out, node.children, 1);
            } else {
                emit_params(out, node.children, 1);
            }
            break;
        }
        case 0x71: {
            if (node.children.empty()) throw std::runtime_error("gm: video command requires selector");
            emit_expression(out, node.children[0]);
            uint32_t selector = require_literal(node.children[0], "video selector");
            if (selector > 7) throw std::runtime_error("gm: video selector must be from 0 through 7");
            if (selector == 0) {
                if (node.children.size() != 2) throw std::runtime_error("gm: video selector 0 needs a reference");
                emit_reference(out, node.children[1]);
                out.emit(0);
                out.emit(0);
            } else if (selector == 1) {
                if (node.children.size() != 2) throw std::runtime_error("gm: video selector 1 needs an expression");
                emit_expression(out, node.children[1]);
                out.emit(0);
            } else {
                emit_params(out, node.children, 1);
            }
            break;
        }
        case 0x72: case 0x73: case 0x7e:
            expect_children(node, 3);
            emit_expression(out, node.children[0]);
            emit_reference(out, node.children[1]);
            out.emit(0);
            emit_expression(out, node.children[2]);
            out.emit(0);
            break;
        case 0x76: case 0x77:
            expect_children(node, 2);
            emit_expression(out, node.children[0]);
            emit_expression(out, node.children[1]);
            out.emit(0);
            break;
        case 0x79:
            if (node.children.empty()) throw std::runtime_error("gm: file-date requires expression");
            emit_expression(out, node.children[0]);
            emit_reference_list(out, node.children, 1);
            break;
        case 0x7b:
            if (node.children.empty()) throw std::runtime_error("gm: hit-test requires reference");
            emit_reference(out, node.children[0]);
            out.emit(0);
            emit_params(out, node.children, 1);
            break;
        default:
            throw std::runtime_error("gm: missing semantic compiler layout");
    }

    return true;
}

} // namespace gm
