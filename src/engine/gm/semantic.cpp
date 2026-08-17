//
// lime-juice: C++ port of Tomyun's "Juice" de/recompiler for PC-98 games
// Copyright (C) 2026 Fuzion
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//

#include "semantic.h"

#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
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
        case 0x20: return "mul";
        case 0x21: return "div";
        case 0x22: return "mod";
        case 0x23: return "add";
        case 0x24: return "sub";
        case 0x25: return "bit-and";
        case 0x26: return "bit-or";
        case 0x28: return "eq";
        case 0x29: return "ne";
        case 0x2a: return "lt";
        case 0x2b: return "le";
        case 0x2c: return "gt";
        case 0x2d: return "ge";
        case 0x2e: return "logical-and";
        case 0x2f: return "logical-or";
        default: return nullptr;
    }
}

bool literal_value(const AstNode& expression, uint32_t& value,
                   size_t* width = nullptr) {
    if (!expression.is_list("gm-expr") || expression.children.size() != 1) {
        return false;
    }

    const auto& literal = expression.children[0];

    if (!literal.is_list("gm-imm") || literal.children.size() != 2 ||
        !literal.children[0].is_integer() || !literal.children[1].is_integer()) {
        return false;
    }

    int literal_width = literal.children[0].int_val;

    if (literal_width < 1 || literal_width > 4) {
        return false;
    }

    value = static_cast<uint32_t>(literal.children[1].int_val);

    if (width != nullptr) {
        *width = static_cast<size_t>(literal_width);
    }

    return true;
}

class Decoder {
public:
    Decoder(const std::vector<uint8_t>& code, size_t code_base,
            const InstructionSpan& instruction)
        : code_(code), base_(code_base), end_(instruction.end - code_base),
          pos_(instruction.start - code_base + 1),
          opcode_(instruction.opcode) {
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

    AstNode bytes(size_t count, const std::string& tag = "gm-bytes") {
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

        return list("gm-ref", std::move(children));
    }

    AstNode operand(bool initial) {
        uint8_t token = peek();

        if (initial && token == 0x32) {
            byte();
            return list("gm-random", {integer(u16()), integer(u16())});
        }

        if (token >= 1 && token <= 4) {
            size_t width = byte();
            uint32_t value = 0;

            for (size_t i = 0; i < width; i++) {
                value |= static_cast<uint32_t>(byte()) << (i * 8);
            }

            return list("gm-imm", {integer(static_cast<uint32_t>(width)),
                                    integer(value)});
        }

        return reference();
    }

    AstNode expression() {
        std::vector<AstNode> terms;

        if (peek() == 0) {
            byte();
            return list("gm-expr");
        }

        terms.push_back(operand(true));

        while (peek() != 0) {
            uint8_t token = peek();

            if (token >= 0x20) {
                byte();
                const char* name = operator_name(token);

                if (name != nullptr) {
                    terms.push_back(AstNode::make_symbol(name));
                } else {
                    terms.push_back(list("gm-op", {integer(token)}));
                }
            } else {
                terms.push_back(operand(false));
            }
        }

        zero();
        return list("gm-expr", std::move(terms));
    }

    std::vector<AstNode> params() {
        std::vector<AstNode> values;

        while (peek() != 0) {
            if (peek() == 0x11) {
                byte();
                std::vector<AstNode> string_bytes;

                while (peek() != 0) {
                    string_bytes.push_back(integer(byte()));
                }

                zero();
                values.push_back(list("gm-string-bytes", std::move(string_bytes)));
            } else if (peek() == 0x0f) {
                values.push_back(list("gm-ref-param", {reference()}));
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
        return list("gm-address", {integer(u16())});
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
                    auto value = expression();
                    size_t width = 0;
                    uint32_t ignored = 0;

                    if (i == 4 && literal_value(value, ignored, &width) && width == 2) {
                        value = list("gm-callback", {std::move(value)});
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
        return list(opcode_name(opcode_), std::move(children));
    }

    AstNode assignment() {
        std::vector<AstNode> children = {reference()};

        while (true) {
            AstNode value;

            if (peek() == 0x0e) {
                value = list("gm-string-value", {reference()});
                zero();
            } else {
                value = expression();
            }

            std::vector<AstNode> strides;

            while (peek() == 0x31) {
                byte();
                strides.push_back(integer(byte()));
            }

            std::vector<AstNode> wrapped = {std::move(value)};

            if (!strides.empty()) {
                wrapped.push_back(list("gm-strides", std::move(strides)));
            }

            children.push_back(list("gm-value", std::move(wrapped)));

            if (peek() == 0) {
                zero();
                break;
            }

            if (peek() == 0x30) {
                byte();
                children.push_back(list("gm-range", {reference()}));
                zero();
                break;
            }
        }

        return list(opcode_name(opcode_), std::move(children));
    }

    AstNode struct_assignment() {
        AstNode destination = reference();
        uint16_t word = u16();

        if (peek() == 0x0f) {
            AstNode source = reference();
            zero();
            return list(opcode_name(opcode_), {
                std::move(destination), integer(word),
                list("gm-ref-source", {std::move(source)})
            });
        }

        std::vector<AstNode> payload;
        size_t target = word;
        size_t absolute_pos = base_ + pos_;

        if (target <= absolute_pos || target != base_ + end_) {
            throw std::runtime_error("gm: inconsistent inline struct target");
        }

        while (pos_ < end_) {
            payload.push_back(integer(byte()));
        }

        return list(opcode_name(opcode_), {
            std::move(destination), list("gm-inline", std::move(payload))
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
            return list(opcode_name(opcode_), {
                std::move(destination),
                list("gm-ref-source", {integer(first), std::move(source)})
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
        return list(opcode_name(opcode_), {
            std::move(destination), list("gm-inline-source", std::move(children))
        });
    }

    AstNode decode_body() {
        const char* name = opcode_name(opcode_);

        if (name == nullptr || opcode_ == 0x4a) {
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
            case 0x33: case 0x34:
                return list(name, {address(), expression()});
            case 0x35: {
                std::vector<AstNode> children = {address()};
                while (peek() != 0) children.push_back(expression());
                zero();
                return list(name, std::move(children));
            }
            case 0x37:
                return list(name, {expression()});
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
    if (!reference.is_list("gm-ref") ||
        (reference.children.size() != 2 && reference.children.size() != 3)) {
        throw std::runtime_error("gm: expected (gm-ref TOKEN OFFSET [INDEX])");
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
    if (node.is_list("gm-op")) {
        expect_children(node, 1);
        int token = checked_integer(node.children[0], "expression operator",
                                    0x20, 0x2f);
        if (token == 0x27) {
            throw std::runtime_error("gm: expression operator 0x27 is invalid");
        }
        return static_cast<uint8_t>(token);
    }

    if (!node.is_symbol()) {
        throw std::runtime_error("gm: expected expression term or operator");
    }

    static const std::unordered_map<std::string, uint8_t> operators = {
        {"mul", 0x20}, {"div", 0x21}, {"mod", 0x22}, {"add", 0x23},
        {"sub", 0x24}, {"bit-and", 0x25}, {"bit-or", 0x26},
        {"eq", 0x28}, {"ne", 0x29}, {"lt", 0x2a}, {"le", 0x2b},
        {"gt", 0x2c}, {"ge", 0x2d}, {"logical-and", 0x2e},
        {"logical-or", 0x2f}
    };
    auto found = operators.find(node.str_val);

    if (found == operators.end()) {
        throw std::runtime_error("gm: unknown expression operator: " + node.str_val);
    }

    return found->second;
}

void emit_expression(ByteWriter& out, const AstNode& expression) {
    if (!expression.is_list("gm-expr")) {
        throw std::runtime_error("gm: expected gm-expr");
    }

    for (size_t i = 0; i < expression.children.size(); i++) {
        const auto& term = expression.children[i];

        if (term.is_list("gm-imm")) {
            expect_children(term, 2);
            int width = checked_integer(term.children[0], "literal width", 1, 4);

            if (!term.children[1].is_integer()) {
                throw std::runtime_error("gm: invalid literal value");
            }

            uint32_t value = static_cast<uint32_t>(term.children[1].int_val);

            if (width < 4 && value >= (1u << (width * 8))) {
                throw std::runtime_error("gm: literal does not fit its width");
            }

            out.emit(static_cast<uint8_t>(width));
            for (int byte_index = 0; byte_index < width; byte_index++) {
                out.emit(static_cast<uint8_t>(value >> (byte_index * 8)));
            }
        } else if (term.is_list("gm-ref")) {
            emit_reference(out, term);
        } else if (term.is_list("gm-random")) {
            if (i != 0) {
                throw std::runtime_error("gm: random range must be the first expression term");
            }
            expect_children(term, 2);
            out.emit(0x32);
            out.emit_u16_le(static_cast<uint16_t>(checked_integer(
                term.children[0], "random lower bound", 0, 65535)));
            out.emit_u16_le(static_cast<uint16_t>(checked_integer(
                term.children[1], "random upper bound", 0, 65535)));
        } else {
            out.emit(operator_token(term));
        }
    }

    out.emit(0);
}

void emit_params(ByteWriter& out, const std::vector<AstNode>& values,
                 size_t first = 0) {
    for (size_t i = first; i < values.size(); i++) {
        const auto& value = values[i];

        if (value.is_list("gm-string-bytes")) {
            out.emit(0x11);
            emit_bytes(out, value, "gm-string-bytes");
            out.emit(0);
        } else if (value.is_list("gm-ref-param")) {
            expect_children(value, 1);
            if (!value.children[0].is_list("gm-ref") ||
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
    if (!address.is_list("gm-address")) {
        throw std::runtime_error("gm: expected gm-address");
    }

    expect_children(address, 1);
    uint16_t target = static_cast<uint16_t>(checked_integer(
        address.children[0], "control address", 0, 65535));
    size_t field = out.size();
    out.emit_u16_le(target);
    relocations.push_back({field, target});
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

        if (value.is_list("gm-ref")) {
            emit_reference(out, value);
            out.emit(0);
        } else if (value.is_list("gm-callback")) {
            expect_children(value, 1);
            uint32_t target = require_literal(value.children[0], "menu callback");
            size_t width = 0;
            uint32_t ignored = 0;

            if (!literal_value(value.children[0], ignored, &width) || width != 2 ||
                target > 65535) {
                throw std::runtime_error("gm: menu callback must be a 16-bit literal");
            }

            size_t expression_start = out.size();
            emit_expression(out, value.children[0]);
            relocations.push_back({expression_start + 1,
                                   static_cast<uint16_t>(target)});
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

        if (child.is_list("gm-range")) {
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

        if (!child.is_list("gm-value") || child.children.empty() ||
            child.children.size() > 2) {
            throw std::runtime_error("gm: malformed assignment value");
        }

        const auto& value = child.children[0];

        if (value.is_list("gm-string-value")) {
            expect_children(value, 1);
            if (!value.children[0].is_list("gm-ref") ||
                value.children[0].children.empty() ||
                !value.children[0].children[0].is_integer() ||
                value.children[0].children[0].int_val != 0x0e) {
                throw std::runtime_error("gm: string assignment must use reference token 0x0e");
            }
            emit_reference(out, value.children[0]);
            out.emit(0);
        } else {
            emit_expression(out, value);
        }

        if (child.children.size() == 2) {
            const auto& strides = child.children[1];
            if (!strides.is_list("gm-strides")) {
                throw std::runtime_error("gm: malformed assignment strides");
            }
            for (const auto& stride : strides.children) {
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
        node.children[2].is_list("gm-ref-source")) {
        emit_reference(out, node.children[0]);
        out.emit_u16_le(static_cast<uint16_t>(checked_integer(
            node.children[1], "struct word", 0, 65535)));
        expect_children(node.children[2], 1);
        if (!node.children[2].children[0].is_list("gm-ref") ||
            node.children[2].children[0].children.empty() ||
            !node.children[2].children[0].children[0].is_integer() ||
            node.children[2].children[0].children[0].int_val != 0x0f) {
            throw std::runtime_error("gm: struct reference source must use token 0x0f");
        }
        emit_reference(out, node.children[2].children[0]);
        out.emit(0);
        return;
    }

    if (node.children.size() == 2 && node.children[1].is_list("gm-inline")) {
        emit_reference(out, node.children[0]);
        size_t field = out.size();
        out.emit_u16_le(0);
        emit_bytes(out, node.children[1], "gm-inline");

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

    if (source.is_list("gm-ref-source")) {
        expect_children(source, 2);
        out.emit(static_cast<uint8_t>(checked_integer(source.children[0],
                                                      "string source prefix", 0, 255)));
        emit_reference(out, source.children[1]);
        out.emit(0);
    } else if (source.is_list("gm-inline-source") && !source.children.empty()) {
        for (size_t i = 1; i < source.children.size(); i++) {
            out.emit(static_cast<uint8_t>(checked_integer(source.children[i],
                                                      "inline string byte", 0, 255)));
        }
        out.emit(0);
        out.emit(static_cast<uint8_t>(checked_integer(source.children[0],
                                                      "string trailing byte", 0, 255)));
    } else {
        throw std::runtime_error("gm: malformed string source");
    }
}

uint8_t opcode_for_tag(const std::string& tag) {
    for (int opcode = 0; opcode <= 0x85; opcode++) {
        const char* name = opcode_name(static_cast<uint8_t>(opcode));
        if (name != nullptr && tag == name) return static_cast<uint8_t>(opcode);
    }
    return 0xff;
}

} // namespace

const char* opcode_name(uint8_t opcode) {
    switch (opcode) {
        case 0x00: return "gm-end";
        case 0x30: return "gm-nop-30";
        case 0x31: return "gm-for-start";
        case 0x32: return "gm-for-continue";
        case 0x33: return "gm-if";
        case 0x34: return "gm-switch";
        case 0x35: return "gm-case";
        case 0x36: return "gm-nop-36";
        case 0x37: return "gm-next";
        case 0x38: return "gm-while-continue";
        case 0x39: return "gm-gosub-if";
        case 0x3a: return "gm-menu";
        case 0x3b: return "gm-eval";
        case 0x3c: return "gm-skip-5";
        case 0x3d: return "gm-menu-click-wait";
        case 0x3e: return "gm-cursor";
        case 0x3f: return "gm-call-reset-if";
        case 0x40: return "gm-gosub-if-save";
        case 0x41: return "gm-return";
        case 0x42: return "gm-nop-42";
        case 0x43: return "gm-assign";
        case 0x44: return "gm-struct-assign";
        case 0x45: return "gm-string-copy";
        case 0x46: return "gm-text-window";
        case 0x47: return "gm-text-origin";
        case 0x48: return "gm-text-window-stack";
        case 0x49: return "gm-text-attribute";
        case 0x4a: return "gm-text";
        case 0x4b: return "gm-text-indirect";
        case 0x4c: return "gm-number";
        case 0x4d: return "gm-text-color";
        case 0x4e: return "gm-palette-color-map";
        case 0x4f: return "gm-text-clear";
        case 0x50: return "gm-message-end";
        case 0x51: return "gm-palette-set";
        case 0x52: return "gm-palette-fill";
        case 0x53: return "gm-palette-work-fill";
        case 0x54: return "gm-palette-apply";
        case 0x55: return "gm-fade-wait";
        case 0x56: return "gm-palette-save";
        case 0x57: return "gm-palette-restore";
        case 0x58: return "gm-display-page";
        case 0x59: return "gm-display-plane";
        case 0x5a: return "gm-image-open";
        case 0x5b: return "gm-fill-rect";
        case 0x5c: return "gm-box-rect";
        case 0x5d: return "gm-blit";
        case 0x5e: return "gm-blit-variant";
        case 0x5f: return "gm-blit-mode";
        case 0x60: return "gm-window-blit-setup";
        case 0x61: return "gm-window-blit-setup-alt";
        case 0x62: return "gm-gdc-window-init";
        case 0x63: return "gm-sprite";
        case 0x64: return "gm-input";
        case 0x65: return "gm-store-values";
        case 0x66: return "gm-wait-key";
        case 0x67: return "gm-delay";
        case 0x68: return "gm-store-six-values";
        case 0x69: return "gm-tick-snapshot";
        case 0x6a: return "gm-tick-delta";
        case 0x6b: return "gm-mouse-command";
        case 0x6c: return "gm-drive-slot";
        case 0x6d: return "gm-mes-jump";
        case 0x6e: return "gm-mll-load";
        case 0x6f: return "gm-mes-call";
        case 0x70: return "gm-loop-end";
        case 0x71: return "gm-video-command";
        case 0x72: return "gm-file-save-range";
        case 0x73: return "gm-file-load-range";
        case 0x74: return "gm-save-slot";
        case 0x75: return "gm-load-slot";
        case 0x76: return "gm-image-load";
        case 0x77: return "gm-image-load-alt";
        case 0x78: return "gm-vram-bank";
        case 0x79: return "gm-file-date";
        case 0x7a: return "gm-music-command";
        case 0x7b: return "gm-hit-test";
        case 0x7c: return "gm-hook";
        case 0x7d: return "gm-driver-state";
        case 0x7e: return "gm-progress-merge";
        case 0x7f: return "gm-nop-7f";
        case 0x80: return "gm-beyond-flag-test";
        case 0x81: return "gm-beyond-external-call";
        case 0x82: return "gm-beyond-bank";
        case 0x83: return "gm-for-end";
        case 0x84: return "gm-push-reference";
        case 0x85: return "gm-pop-reference";
        default: return nullptr;
    }
}

AstNode decode_instruction(const std::vector<uint8_t>& code, size_t code_base,
                           const InstructionSpan& instruction) {
    try {
        return Decoder(code, code_base, instruction).decode();
    } catch (const std::exception& error) {
        throw std::runtime_error(
            "gm: semantic opcode " + std::to_string(instruction.opcode) +
            " at " + std::to_string(instruction.start) + ": " + error.what());
    }
}

bool emit_instruction(ByteWriter& out, const AstNode& node,
                      std::vector<SemanticRelocation>& relocations) {
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
        case 0x33: case 0x34:
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
            expect_children(node, 1);
            emit_expression(out, node.children[0]);
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
            if (!node.children[0].is_list("gm-bytes") || node.children[0].children.size() != 5)
                throw std::runtime_error("gm: skip-5 requires five bytes");
            emit_bytes(out, node.children[0], "gm-bytes");
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
        case 0x4b: case 0x84: case 0x85:
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
