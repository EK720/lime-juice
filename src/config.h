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

#pragma once

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

enum class EngineType {
    AI5,
    AI1,
    ADV,
    AI5WIN,
    GM
};

// protagonist fusion specification
// controls whether proc/call instructions get fused into text
struct ProtagSpec {
    bool all = false;                               // --protag all
    std::set<std::variant<int, char>> entries;      // --protag 0,Z,...

    bool matches(int proc_id) const;
    bool matches(char var_name) const;
};

struct Config {
    // general
    std::string preset;
    EngineType engine = EngineType::AI5;
    std::string charset_name = "pc98";

    // special characters (set by charset)
    char32_t char_space    = U'\u3000';
    char32_t char_newline  = U'\uFF05';  // fullwidth %
    char32_t char_continue = U'\uFF20';  // fullwidth @
    char32_t char_break    = U'\uFF03';  // fullwidth #
    int fontwidth = 2;

    // engine parameters
    bool use_dict = true;
    uint8_t dict_base = 0x80;
    bool extra_op = false;
    bool decrypt_op = true;

    // decompile options
    bool decode = true;
    bool resolve = true;
    std::optional<ProtagSpec> protag;

    // compile options
    std::optional<int> wordwrap;
    bool compress = true;

    // apply a named game preset
    void use_preset(const std::string& name);

    // set engine type from string (handles aliases: AI2->AI1, AI4->AI5, AI5X->AI5+dictD0+extraop)
    void set_engine(const std::string& name);

    // parse protag specification string (e.g. "all", "none", "0", "Z", "0,Z")
    void set_protag(const std::string& spec);

    // check if a proc/call should be fused into text
    bool is_protag(int proc_id) const;
    bool is_protag(char var_name) const;
};

struct PresetInfo {
    std::string key;
    std::string title;
};

// get the list of all supported presets for display
std::vector<PresetInfo> get_presets();
