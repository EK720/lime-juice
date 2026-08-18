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

#include "walker.h"

#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>

namespace gm {

namespace {

struct Expression {
    size_t end;
    bool pure_literal = false;
    uint32_t literal = 0;
    size_t literal_offset = 0;
    size_t literal_width = 0;
};

struct PendingRelocation {
    size_t field;
    uint16_t target;
    bool required_local;
    bool control_flow = true;
};

struct ParsedInstruction {
    InstructionSpan span;
    std::vector<PendingRelocation> relocations;
};

class Walker {
public:
    Walker(const std::vector<uint8_t>& code, size_t code_base)
        : code_(code), base_(code_base), end_(code_base + code.size()) {
        if (end_ < base_) {
            throw std::runtime_error("gm: code range overflow");
        }

        if (end_ > 0x10000) {
            throw std::runtime_error("gm: MES file exceeds the 16-bit address space");
        }
    }

    WalkResult walk() {
        WalkResult result;
        std::vector<PendingRelocation> pending;
        size_t pos = base_;

        while (pos < end_) {
            auto instruction = read_instruction(pos);

            if (instruction.span.end <= pos) {
                fail(pos, "instruction did not advance");
            }

            pos = instruction.span.end;
            result.instructions.push_back(instruction.span);
            pending.insert(pending.end(), instruction.relocations.begin(),
                           instruction.relocations.end());
        }

        if (pos != end_) {
            fail(pos, "instruction extends past the end of the MES code");
        }

        std::unordered_set<size_t> boundaries;

        for (const auto& instruction : result.instructions) {
            boundaries.insert(instruction.start);
        }

        for (const auto& relocation : pending) {
            bool local = relocation.target >= base_ && relocation.target < end_;

            if (relocation.required_local && !local) {
                fail(relocation.field, "control target is outside the MES code");
            }

            if (local && boundaries.count(relocation.target) == 0) {
                fail(relocation.field, "control target is not an instruction boundary");
            }

            if (local && relocation.control_flow) {
                result.relocations.push_back({relocation.field, relocation.target});
            }
        }

        return result;
    }

private:
    struct Operand {
        size_t end;
        bool literal = false;
        uint32_t value = 0;
        size_t width = 0;
    };

    [[noreturn]] void fail(size_t pos, const std::string& message) const {
        std::ostringstream out;
        out << "gm: 0x" << std::hex << std::setw(4) << std::setfill('0') << pos
            << ": " << message;
        throw std::runtime_error(out.str());
    }

    void require(size_t pos, size_t size, const std::string& what) const {
        if (pos < base_ || pos > end_ || size > end_ - pos) {
            fail(pos, "truncated " + what);
        }
    }

    uint8_t byte(size_t pos, const std::string& what = "operand") const {
        require(pos, 1, what);
        return code_[pos - base_];
    }

    uint16_t u16(size_t pos) const {
        require(pos, 2, "16-bit operand");
        return static_cast<uint16_t>(code_[pos - base_] |
                                     (code_[pos - base_ + 1] << 8));
    }

    size_t find_zero(size_t pos, const std::string& what) const {
        while (pos < end_ && code_[pos - base_] != 0) {
            pos++;
        }

        if (pos == end_) {
            fail(pos, "unterminated " + what);
        }

        return pos;
    }

    size_t expect_zero(size_t pos, const std::string& what) const {
        uint8_t found = byte(pos, what);

        if (found != 0) {
            std::ostringstream message;
            message << "expected zero " << what << ", found 0x" << std::hex
                    << std::setw(2) << std::setfill('0') << static_cast<int>(found);
            fail(pos, message.str());
        }

        return pos + 1;
    }

    size_t read_reference(size_t pos) const {
        require(pos, 3, "reference");
        uint8_t token = byte(pos);

        if (token < 5 || token > 0x12) {
            fail(pos, "invalid reference token");
        }

        pos += 3;

        if (token <= 0x0a) {
            pos = read_expression(pos).end;
        }

        return pos;
    }

    Operand read_operand(size_t pos, bool initial) const {
        uint8_t token = byte(pos, "expression operand");

        if (initial && token == 0x32) {
            require(pos, 5, "random-range operand");
            return {pos + 5};
        }

        if (token >= 1 && token <= 4) {
            require(pos + 1, token, "literal operand");
            uint32_t value = 0;

            for (size_t i = 0; i < token; i++) {
                value |= static_cast<uint32_t>(byte(pos + 1 + i)) << (i * 8);
            }

            return {pos + 1 + token, true, value, token};
        }

        if (token >= 5) {
            return {read_reference(pos)};
        }

        fail(pos, "zero is not an expression operand");
    }

    Expression read_expression(size_t pos) const {
        size_t start = pos;

        if (byte(pos, "expression") == 0) {
            return {pos + 1};
        }

        auto operand = read_operand(pos, true);
        pos = operand.end;
        bool pure_literal = operand.literal;

        while (true) {
            uint8_t token = byte(pos, "expression terminator");

            if (token == 0) {
                Expression result{pos + 1};

                if (pure_literal) {
                    result.pure_literal = true;
                    result.literal = operand.value;
                    result.literal_offset = start + 1;
                    result.literal_width = operand.width;
                }

                return result;
            }

            pure_literal = false;

            if (token >= 0x20 && token <= 0x2f && token != 0x27) {
                pos++;
            } else if (token >= 0x20) {
                fail(pos, "invalid expression operator");
            } else {
                pos = read_operand(pos, false).end;
            }
        }
    }

    size_t read_params(size_t pos) const {
        while (true) {
            uint8_t token = byte(pos, "parameter list");

            if (token == 0) {
                return pos + 1;
            }

            if (token == 0x11) {
                pos = find_zero(pos + 1, "literal-string parameter") + 1;
            } else if (token == 0x0f) {
                pos = read_reference(pos);
                pos = expect_zero(pos, "after reference parameter");
            } else {
                pos = read_expression(pos).end;
            }
        }
    }

    size_t read_reference_list(size_t pos) const {
        while (true) {
            if (byte(pos, "reference list") == 0) {
                return pos + 1;
            }

            pos = read_reference(pos);
            pos = expect_zero(pos, "after reference");
        }
    }

    size_t read_assignment(size_t pos) const {
        pos = read_reference(pos);

        while (true) {
            if (byte(pos) == 0x0e) {
                pos = read_reference(pos);
                pos = expect_zero(pos, "after string reference");
            } else {
                pos = read_expression(pos).end;
            }

            while (byte(pos) == 0x31) {
                require(pos, 2, "assignment stride");
                pos += 2;
            }

            if (byte(pos) == 0) {
                return pos + 1;
            }

            if (byte(pos) == 0x30) {
                pos = read_reference(pos + 1);
                return expect_zero(pos, "after assignment range");
            }
        }
    }

    void add_relocation(std::vector<PendingRelocation>& relocations, size_t field,
                        bool required_local) const {
        relocations.push_back({field, u16(field), required_local});
    }

    size_t read_3a(size_t pos, std::vector<PendingRelocation>& relocations) const {
        uint8_t subtype = byte(pos, "opcode 0x3a subtype");
        pos++;

        if (subtype == 1) {
            auto selector = read_expression(pos);
            pos = selector.end;
            pos = read_expression(pos).end;

            if (selector.pure_literal && selector.literal == 1) {
                for (size_t field = 0; field < 6; field++) {
                    auto value = read_expression(pos);

                    if (field == 4 && value.pure_literal &&
                        value.literal_width == 2) {
                        relocations.push_back({value.literal_offset,
                                              static_cast<uint16_t>(value.literal), true});
                    }

                    pos = value.end;
                }
            }
        } else if (subtype == 2 || subtype == 3 || subtype == 6) {
            pos = read_reference(pos);
            pos = expect_zero(pos, "after 0x3a reference");
        } else if (subtype == 4) {
            for (size_t i = 0; i < 9; i++) {
                pos = read_expression(pos).end;
            }
        } else if (subtype == 5) {
            for (size_t i = 0; i < 3; i++) {
                pos = read_expression(pos).end;
            }

            for (size_t i = 0; i < 2; i++) {
                pos = read_reference(pos);
                pos = expect_zero(pos, "after 0x3a reference");
            }
        } else if (subtype == 7 || subtype == 8 || subtype == 9) {
            pos = read_expression(pos).end;
        } else if (subtype == 10) {
            auto selector = read_expression(pos);
            pos = selector.end;
            size_t count = selector.pure_literal && selector.literal == 0 ? 7 : 6;

            for (size_t i = 0; i < count; i++) {
                pos = read_reference(pos);
                pos = expect_zero(pos, "after 0x3a subtype 10 reference");
            }

            if (!selector.pure_literal && byte(pos) != 0) {
                pos = read_reference(pos);
                pos = expect_zero(pos, "after 0x3a subtype 10 reference");
            }
        } else if (subtype < 11 || subtype > 12) {
            fail(pos - 1, "unknown opcode 0x3a subtype");
        }

        return expect_zero(pos, "after opcode 0x3a");
    }

    static bool no_operands(uint8_t opcode) {
        switch (opcode) {
            case 0x30: case 0x36: case 0x38: case 0x3d: case 0x41:
            case 0x42: case 0x50: case 0x56: case 0x57: case 0x66:
            case 0x69: case 0x70: case 0x7c: case 0x7f: case 0x80:
                return true;
            default:
                return false;
        }
    }

    static bool generic_params(uint8_t opcode) {
        switch (opcode) {
            case 0x46: case 0x47: case 0x48: case 0x49: case 0x4c:
            case 0x4d: case 0x4e: case 0x4f: case 0x51: case 0x52:
            case 0x53: case 0x54: case 0x55: case 0x58: case 0x59:
            case 0x5a: case 0x5b: case 0x5c: case 0x5d: case 0x5e:
            case 0x5f: case 0x60: case 0x61: case 0x63: case 0x67:
            case 0x6c: case 0x6d: case 0x6e: case 0x6f: case 0x74:
            case 0x75: case 0x78: case 0x7a: case 0x7d: case 0x81:
            case 0x82:
                return true;
            default:
                return false;
        }
    }

    ParsedInstruction read_instruction(size_t start) const {
        uint8_t opcode = byte(start, "opcode");
        size_t pos = start + 1;
        std::vector<PendingRelocation> relocations;

        if (opcode == 0) {
            return {{start, pos, opcode}, {}};
        }

        if (opcode < 0x30 || opcode > 0x85) {
            fail(start, "invalid GM opcode");
        }

        if (no_operands(opcode)) {
            // Nothing to decode.
        } else if (opcode == 0x31 || opcode == 0x83) {
            require(pos, 4, "opcode 0x31 operands");
            add_relocation(relocations, pos + 2, true);
            pos = read_expression(pos + 4).end;
        } else if (opcode == 0x32) {
            require(pos, 4, "opcode 0x32 operands");
            add_relocation(relocations, pos + 2, true);
            pos += 4;
        } else if (opcode == 0x33 || opcode == 0x34) {
            add_relocation(relocations, pos, true);
            pos = read_expression(pos + 2).end;
        } else if (opcode == 0x35) {
            add_relocation(relocations, pos, true);
            pos += 2;

            while (byte(pos) != 0) {
                pos = read_expression(pos).end;
            }

            pos++;
        } else if (opcode == 0x37) {
            pos = read_expression(pos).end;
        } else if (opcode == 0x39 || opcode == 0x3f || opcode == 0x40) {
            require(pos, 4, "call targets");
            add_relocation(relocations, pos, true);
            add_relocation(relocations, pos + 2, false);
            pos = read_expression(pos + 4).end;
        } else if (opcode == 0x3a) {
            pos = read_3a(pos, relocations);
        } else if (opcode == 0x3b) {
            pos = read_expression(pos).end;
            pos = expect_zero(pos, "after opcode 0x3b");
        } else if (opcode == 0x3c) {
            require(pos, 5, "opcode 0x3c payload");
            pos += 5;
        } else if (opcode == 0x3e) {
            require(pos, 1, "opcode 0x3e mode");
            if (byte(pos) != 1 && byte(pos) != 2) {
                fail(pos, "opcode 0x3e mode is not 1 or 2");
            }
            pos = expect_zero(pos + 1, "after opcode 0x3e");
        } else if (opcode == 0x43) {
            pos = read_assignment(pos);
        } else if (opcode == 0x44) {
            pos = read_reference(pos);
            size_t field = pos;
            uint16_t target = u16(field);
            pos += 2;

            if (byte(pos) == 0x0f) {
                pos = read_reference(pos);
                pos = expect_zero(pos, "after opcode 0x44 reference");
            } else {
                // The word only delimits the inline payload. Validate it as
                // an instruction boundary, but do not present it as a control
                // relocation or synthesize an unreferenced label for it.
                relocations.push_back({field, target, true, false});

                if (target <= pos || target > end_) {
                    fail(field, "invalid opcode 0x44 skip target");
                }

                pos = target;
            }
        } else if (opcode == 0x45) {
            pos = read_reference(pos);
            require(pos, 2, "opcode 0x45 source");

            if (byte(pos + 1) >= 5) {
                pos = read_reference(pos + 1);
                pos = expect_zero(pos, "after opcode 0x45 reference");
            } else {
                size_t end = find_zero(pos, "opcode 0x45 inline source");
                require(end + 1, 1, "opcode 0x45 trailing byte");
                pos = end + 2;
            }
        } else if (opcode == 0x4a) {
            uint8_t mode = byte(pos, "opcode 0x4a mode");

            if (mode != 1 && mode != 2) {
                fail(pos, "invalid opcode 0x4a mode");
            }

            pos = find_zero(pos + 1, "opcode 0x4a text") + 1;
        } else if (opcode == 0x4b || opcode == 0x84 || opcode == 0x85) {
            pos = read_reference(pos);
            require(pos, 2, "opcode 0x4b terminators");
            pos += 2;
        } else if (generic_params(opcode)) {
            pos = read_params(pos);
        } else if (opcode == 0x62) {
            pos = expect_zero(pos, "after opcode 0x62");
        } else if (opcode == 0x64) {
            pos = read_reference(pos);
            pos = expect_zero(pos, "after opcode 0x64 reference");
            pos = expect_zero(pos, "after opcode 0x64");
        } else if (opcode == 0x65 || opcode == 0x68 || opcode == 0x6a) {
            pos = read_reference_list(pos);
        } else if (opcode == 0x6b) {
            auto selector = read_expression(pos);
            pos = selector.end;

            if (!selector.pure_literal || selector.literal > 9) {
                fail(start, "opcode 0x6b selector is not a literal from 0 through 9");
            }

            if (selector.literal == 0 || selector.literal == 1 ||
                selector.literal == 7 || selector.literal == 8) {
                pos = expect_zero(pos, "after opcode 0x6b");
            } else if (selector.literal == 2 || selector.literal == 3) {
                pos = read_reference_list(pos);
            } else {
                pos = read_params(pos);
            }
        } else if (opcode == 0x71) {
            auto selector = read_expression(pos);
            pos = selector.end;

            if (!selector.pure_literal || selector.literal > 7) {
                fail(start, "opcode 0x71 selector is not a literal from 0 through 7");
            }

            if (selector.literal == 0) {
                pos = read_reference(pos);
                pos = expect_zero(pos, "after opcode 0x71 reference");
                pos = expect_zero(pos, "after opcode 0x71");
            } else if (selector.literal == 1) {
                pos = read_expression(pos).end;
                pos = expect_zero(pos, "after opcode 0x71");
            } else {
                pos = read_params(pos);
            }
        } else if (opcode == 0x72 || opcode == 0x73 || opcode == 0x7e) {
            pos = read_expression(pos).end;
            pos = read_reference(pos);
            pos = expect_zero(pos, "after reference");
            pos = read_expression(pos).end;
            pos = expect_zero(pos, "after opcode");
        } else if (opcode == 0x76 || opcode == 0x77) {
            pos = read_expression(pos).end;
            pos = read_expression(pos).end;
            pos = expect_zero(pos, "after opcode");
        } else if (opcode == 0x79) {
            pos = read_expression(pos).end;
            pos = read_reference_list(pos);
        } else if (opcode == 0x7b) {
            pos = read_reference(pos);
            pos = expect_zero(pos, "after opcode 0x7b reference");
            pos = read_params(pos);
        } else {
            fail(start, "unsupported GM opcode");
        }

        if (pos > end_) {
            fail(start, "instruction extends past the end of the MES code");
        }

        return {{start, pos, opcode}, std::move(relocations)};
    }

    const std::vector<uint8_t>& code_;
    size_t base_;
    size_t end_;
};

} // namespace

WalkResult walk_code(const std::vector<uint8_t>& code, size_t code_base) {
    return Walker(code, code_base).walk();
}

} // namespace gm
