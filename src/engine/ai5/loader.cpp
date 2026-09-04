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
#include "opener.h"
#include "parser.h"
#include "../ai1/loader.h"
#include "../../charset.h"
#include "../../utf8.h"

#include <algorithm>
#include <cassert>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace ai5 {

// forward declarations for engine-specific helpers
static AstNode lower_impl(const AstNode& node, const Config& cfg, const Charset& cs);
static AstNode lower_chr(const std::vector<int>& sjis_bytes, const Config& cfg, const Charset& cs);
static std::string resolve_exp(int byte_val);
static std::string resolve_cmd(int byte_val, bool do_resolve);
static AstNode resolve_sys(const std::vector<AstNode>& children, bool do_resolve);
static AstNode resolve_impl(const AstNode& node, const Config& cfg);

// AI5-specific fuse passes
static AstNode fuse_dic(const AstNode& node, const std::vector<std::vector<int>>& dict, const Config& cfg, const Charset& cs);
static AstNode fuse_dict_pass(const AstNode& node, const std::vector<std::vector<int>>& dict, const Config& cfg, const Charset& cs);
static AstNode fuse_meta(const AstNode& node, const Config& cfg);
// shared fuse passes: fold_expr, fuse_while, fuse_operator, fuse_text,
// fuse_text_proc_call, fuse_text_number, fuse_text_multiple,
// fuse_text_color, fuse_menu_block are in ai1::loader.h

// --- main pipeline ---

AstNode load_mes(const std::string& path, Config& cfg) {
    Charset cs;
    cs.load(cfg.charset_name);

    // update config with charset's special characters
    cfg.char_space = cs.space_char();
    cfg.char_newline = cs.newline_char();

    auto mes = open_mes(path);
    ByteStream stream(mes.code);
    Parser parser(stream, cfg);
    auto parsed = parser.parse_mes();

    auto lowered = lower(parsed, cfg, cs);
    auto resolved = resolve(lowered, cfg);
    return fuse(resolved, mes.dictionary, cfg, cs);
}

// --- lower ---

AstNode lower(const AstNode& node, const Config& cfg, const Charset& cs) {
    return lower_impl(node, cfg, cs);
}

static AstNode lower_impl(const AstNode& node, const Config& cfg, const Charset& cs) {
    // translated from the (lower l) function in mes-loader.rkt

    if (node.is_list("num")) {
        // (num n) → just n
        if (!node.children.empty() && node.children[0].is_integer()) {
            return node.children[0];
        }
        return node;
    }

    if (node.is_list("var")) {
        // (var byte) → variable symbol (A-Z)
        if (!node.children.empty() && node.children[0].is_integer()) {
            int byte_val = node.children[0].int_val;
            char var_name = static_cast<char>(byte_val);  // 0x41='A' to 0x5A='Z'
            return AstNode::make_variable(var_name);
        }
        return node;
    }

    if (node.is_list("chr-raw")) {
        // (chr-raw j1 j2) → decoded character or kept raw
        std::vector<int> sjis_bytes;

        for (const auto& child : node.children) {

            if (child.is_integer()) {
                sjis_bytes.push_back(child.int_val);
            }
        }

        return lower_chr(sjis_bytes, cfg, cs);
    }

    if (node.is_list("exprs")) {
        // (exprs e ...) → lower the children directly
        std::vector<AstNode> lowered;

        for (const auto& child : node.children) {
            lowered.push_back(lower_impl(child, cfg, cs));
        }

        // flatten: if this is at the end of another list, the racket code flattens it
        // but here we just return lowered children as a list
        return AstNode::make_list("exprs", std::move(lowered));
    }

    if (node.is_list("expr")) {
        // (expr terms...) → fold-expr on lowered terms
        std::vector<AstNode> lowered_terms;

        for (const auto& child : node.children) {
            lowered_terms.push_back(lower_impl(child, cfg, cs));
        }

        return ai1::fold_expr(lowered_terms);
    }

    if (node.is_list("param")) {
        // (param p) → lower the inner value
        if (node.children.size() == 1) {
            return lower_impl(node.children[0], cfg, cs);
        }
        return node;
    }

    if (node.is_list("<*>")) {
        // (<*> s) → lower the single child if there's only one
        if (node.children.size() == 1) {
            return lower_impl(node.children[0], cfg, cs);
        }

        // otherwise lower all children
        std::vector<AstNode> lowered;

        for (const auto& child : node.children) {
            lowered.push_back(lower_impl(child, cfg, cs));
        }

        return AstNode::make_list("<*>", std::move(lowered));
    }

    if (node.is_list()) {
        // for any other list, check for the pattern:
        // (c ... (exprs e ...)) → flatten exprs into the parent list
        // and (c (params p ...)) → flatten params
        std::vector<AstNode> lowered;

        for (size_t i = 0; i < node.children.size(); i++) {
            const auto& child = node.children[i];

            if (child.is_list("exprs") && i == node.children.size() - 1) {
                // trailing exprs: flatten into parent
                for (const auto& expr_child : child.children) {
                    lowered.push_back(lower_impl(expr_child, cfg, cs));
                }
            } else if (child.is_list("params")) {
                // params: flatten into parent
                for (const auto& param_child : child.children) {
                    lowered.push_back(lower_impl(param_child, cfg, cs));
                }
            } else {
                lowered.push_back(lower_impl(child, cfg, cs));
            }
        }

        return AstNode::make_list(node.tag, std::move(lowered));
    }

    // atoms pass through unchanged
    return node;
}

static AstNode lower_chr(const std::vector<int>& sjis_bytes, const Config& cfg, const Charset& cs) {
    auto opt_char = cs.sjis_to_char(sjis_bytes);

    if (opt_char.has_value()) {
        char32_t c = *opt_char;

        // check for newline character
        if (c == cfg.char_newline) {
            c = U'\n';
        }

        if (cfg.decode) {
            return AstNode::make_list("chr", {AstNode::make_character(c)});
        }
    }

    // keep raw if decode is off or conversion failed
    std::vector<AstNode> children;

    for (int b : sjis_bytes) {
        children.push_back(AstNode::make_integer(b));
    }

    return AstNode::make_list("chr-raw", std::move(children));
}

// --- resolve ---

AstNode resolve(const AstNode& node, const Config& cfg) {
    return resolve_impl(node, cfg);
}

static AstNode resolve_impl(const AstNode& node, const Config& cfg) {
    if (node.is_list("exp")) {
        // (exp byte_val ...) → replace byte with operator symbol
        if (!node.children.empty() && node.children[0].is_integer()) {
            std::string op_name = resolve_exp(node.children[0].int_val);
            std::vector<AstNode> new_children;
            new_children.push_back(AstNode::make_symbol(op_name));

            for (size_t i = 1; i < node.children.size(); i++) {
                new_children.push_back(resolve_impl(node.children[i], cfg));
            }

            // return as a symbol-tagged expression
            return AstNode::make_list(op_name, std::vector<AstNode>(new_children.begin() + 1, new_children.end()));
        }
    }

    if (node.is_list("cmd")) {
        // (cmd byte_val params...) → replace byte with command name
        if (!node.children.empty() && node.children[0].is_integer()) {
            std::string cmd_name = resolve_cmd(node.children[0].int_val, cfg.resolve);
            std::vector<AstNode> new_children;

            for (size_t i = 1; i < node.children.size(); i++) {
                new_children.push_back(resolve_impl(node.children[i], cfg));
            }

            return AstNode::make_list(cmd_name, std::move(new_children));
        }
    }

    if (node.is_list("sys")) {
        // (sys byte_val [extra] params...) → replace byte with sys function name
        // then recursively resolve children (params may contain nested exp/cmd/sys)
        auto result = resolve_sys(node.children, cfg.resolve);

        std::vector<AstNode> resolved_children;
        for (const auto& child : result.children) {
            resolved_children.push_back(resolve_impl(child, cfg));
        }

        result.children = std::move(resolved_children);
        return result;
    }

    if (node.is_list()) {
        // recurse into other list nodes
        std::vector<AstNode> new_children;

        for (const auto& child : node.children) {
            new_children.push_back(resolve_impl(child, cfg));
        }

        return AstNode::make_list(node.tag, std::move(new_children));
    }

    return node;
}

static std::string resolve_exp(int byte_val) {
    switch (byte_val) {
        case 0x20: return "+";
        case 0x21: return "-";
        case 0x22: return "*";
        case 0x23: return "/";
        case 0x24: return "%";
        case 0x25: return "//";
        case 0x26: return "&&";
        case 0x27: return "==";
        case 0x28: return "!=";
        case 0x29: return ">";
        case 0x2A: return "<";
        case 0x2B: return "~";
        case 0x2C: return "~b";
        case 0x2D: return ":";
        case 0x2E: return "::";
        case 0x2F: return "?";
        default:   return "exp:" + std::to_string(byte_val);
    }
}

static std::string resolve_cmd(int byte_val, bool do_resolve) {
    if (!do_resolve) {
        return "cmd:" + std::to_string(byte_val);
    }

    switch (byte_val) {
        case 0x10: return "text-color";
        case 0x11: return "wait";
        case 0x12: return "define-proc";
        case 0x13: return "proc";
        case 0x14: return "call";
        case 0x15: return "number";
        case 0x16: return "delay";
        case 0x17: return "clear";
        case 0x18: return "color";
        case 0x19: return "util";
        case 0x1A: return "animate";
        default:   return "cmd:" + std::to_string(byte_val);
    }
}

static AstNode resolve_sys(const std::vector<AstNode>& children, bool do_resolve) {
    if (children.empty()) {
        return AstNode::make_list("sys", {});
    }

    int byte_val = -1;

    if (children[0].is_integer()) {
        byte_val = children[0].int_val;
    }

    std::string sys_name;

    if (!do_resolve || byte_val < 0) {
        sys_name = "sys:" + std::to_string(byte_val);
    } else {
        switch (byte_val) {
            case 0x10: sys_name = "while"; break;
            case 0x11: sys_name = "continue"; break;
            case 0x12: sys_name = "break"; break;
            case 0x13: sys_name = "menu-show"; break;
            case 0x14: sys_name = "menu-init"; break;
            case 0x15: sys_name = "mouse"; break;
            case 0x16: sys_name = "palette"; break;
            case 0x17: sys_name = "box"; break;
            case 0x18: sys_name = "box-inv"; break;
            case 0x19: sys_name = "blit"; break;
            case 0x1A: sys_name = "blit-swap"; break;
            case 0x1B: sys_name = "blit-mask"; break;
            case 0x1C: sys_name = "load"; break;
            case 0x1D: sys_name = "image"; break;
            case 0x1E: sys_name = "mes-jump"; break;
            case 0x1F: sys_name = "mes-call"; break;
            case 0x21: sys_name = "flag"; break;
            case 0x22: sys_name = "slot"; break;
            case 0x23: sys_name = "click"; break;
            case 0x24: sys_name = "sound"; break;
            case 0x26: sys_name = "field"; break;
            default:   sys_name = "sys:" + std::to_string(byte_val); break;
        }
    }

    // build result: (sys_name extra_num? params...)
    std::vector<AstNode> result_children;

    // check for extra num (esoteric opcode from isaku/yuno)
    size_t params_start = 1;

    if (children.size() > 1 && children[1].is_list("num")) {
        // extra number parameter
        result_children.push_back(children[1].children.empty()
            ? children[1]
            : children[1].children[0]);
        params_start = 2;
    }

    // remaining children are params
    for (size_t i = params_start; i < children.size(); i++) {
        result_children.push_back(children[i]);
    }

    return AstNode::make_list(sys_name, std::move(result_children));
}

// --- fuse ---

AstNode fuse(const AstNode& node, const std::vector<std::vector<int>>& dict, const Config& cfg, const Charset& cs) {
    // apply all 10 fuse passes in order
    auto result = node;
    result = ai1::fuse_while(result);
    result = ai1::fuse_operator(result);
    result = fuse_dic(result, dict, cfg, cs);
    result = ai1::fuse_text(result);
    result = ai1::fuse_text_proc_call(result, cfg);
    result = ai1::fuse_text_number(result);
    result = ai1::fuse_text_multiple(result);
    result = ai1::fuse_text_color(result);
    result = ai1::fuse_menu_block(result);
    result = fuse_dict_pass(result, dict, cfg, cs);
    result = fuse_meta(result, cfg);
    return result;
}

// --- fuse passes ---

static AstNode fuse_dic(const AstNode& node, const std::vector<std::vector<int>>& dict,
                         const Config& cfg, const Charset& cs) {
    // translated from fuse-dic in mes-loader.rkt
    // resolve dictionary references: (dic index) → decoded character

    if (node.is_list("dic")) {

        if (!node.children.empty() && node.children[0].is_integer()) {
            int idx = node.children[0].int_val;

            if (idx < 0 || idx >= static_cast<int>(dict.size())) {
                throw std::runtime_error(
                    "dict index " + std::to_string(idx) + " >= dict size " +
                    std::to_string(dict.size()) + "; try `--dictbase D0`");
            }

            return lower_chr(dict[idx], cfg, cs);
        }
    }

    if (node.is_list()) {
        std::vector<AstNode> new_children;

        for (const auto& child : node.children) {
            new_children.push_back(fuse_dic(child, dict, cfg, cs));
        }

        return AstNode::make_list(node.tag, std::move(new_children));
    }

    return node;
}

static AstNode fuse_dict_pass(const AstNode& node, const std::vector<std::vector<int>>& dict,
                               const Config& cfg, const Charset& cs) {
    // translated from fuse-dict in mes-loader.rkt
    // add dictionary to the mes node as (dict char1 char2 ...)

    if (!node.is_list("mes")) {
        return node;
    }

    // build dict node
    std::vector<AstNode> dict_chars;

    for (const auto& entry : dict) {
        auto opt_char = cs.sjis_to_char(entry);

        if (opt_char.has_value() && cfg.decode) {
            dict_chars.push_back(AstNode::make_character(*opt_char));
        } else {
            // keep as quoted SJIS pair
            std::vector<AstNode> pair;

            for (int b : entry) {
                pair.push_back(AstNode::make_integer(b));
            }

            dict_chars.push_back(AstNode::make_quote(
                AstNode::make_list("_sjis_", std::move(pair))
            ));
        }
    }

    // prepend dict to the mes children
    std::vector<AstNode> new_children;
    new_children.push_back(AstNode::make_list("dict", std::move(dict_chars)));

    for (auto& child : node.children) {
        new_children.push_back(child);
    }

    return AstNode::make_list("mes", std::move(new_children));
}

static AstNode fuse_meta(const AstNode& node, const Config& cfg) {
    // translated from fuse-meta in mes-loader.rkt
    // add metadata to the mes node

    if (!node.is_list("mes")) {
        return node;
    }

    // build meta entries
    std::vector<AstNode> meta_entries;

    // engine
    std::string engine_name;

    switch (cfg.engine) {
        case EngineType::AI5: engine_name = "AI5"; break;
        case EngineType::AI1: engine_name = "AI1"; break;
        case EngineType::ADV: engine_name = "ADV"; break;
        case EngineType::AI5WIN: engine_name = "AI5WIN"; break;
        case EngineType::GM: engine_name = "GM"; break;
    }

    meta_entries.push_back(AstNode::make_list("engine", {
        AstNode::make_quote(AstNode::make_symbol(engine_name))
    }));

    // charset
    meta_entries.push_back(AstNode::make_list("charset", {
        AstNode::make_string(cfg.charset_name)
    }));

    // dictbase
    meta_entries.push_back(AstNode::make_list("dictbase", {
        AstNode::make_integer(cfg.dict_base)
    }));

    // extraop (only if true)
    if (cfg.extra_op) {
        meta_entries.push_back(AstNode::make_list("extraop", {
            AstNode::make_boolean(true)
        }));
    }

    // prepend meta to the mes children (after dict if present)
    std::vector<AstNode> new_children;
    new_children.push_back(AstNode::make_list("meta", std::move(meta_entries)));

    for (auto& child : node.children) {
        new_children.push_back(std::move(child));
    }

    return AstNode::make_list("mes", std::move(new_children));
}

} // namespace ai5
