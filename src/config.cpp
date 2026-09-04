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

#include "config.h"

#include <algorithm>
#include <stdexcept>
#include <sstream>

// --- ProtagSpec ---

bool ProtagSpec::matches(int proc_id) const {
    if (all) {
        return true;
    }
    return entries.count(proc_id) > 0;
}

bool ProtagSpec::matches(char var_name) const {
    if (all) {
        return true;
    }
    return entries.count(var_name) > 0;
}

// --- Config ---

void Config::use_preset(const std::string& name) {
    preset = name;
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    // presets from mes-config.rkt lines 90-135
    if (lower == "aishi" || lower == "cre" || lower == "coc" ||
        lower == "deja2" || lower == "dk4" || lower == "foxy" ||
        lower == "foxy2" || lower == "kawa" || lower == "metal" || 
        lower == "metal2" || lower == "nono" || lower == "pre" ||
        lower == "pre2" || lower == "reira" || lower == "syan" ||
        lower == "syan2" || lower == "ten" || lower == "shima") {
        // these presets use defaults (no changes)
        return;
    }

    if (lower == "fermion" || lower == "beyond" || lower == "be-yond") {
        engine = EngineType::GM;

    } else if (lower == "angel") {
        engine = EngineType::AI1;
    } else if (lower == "deja") {
        engine = EngineType::AI1;
        ProtagSpec ps;
        ps.entries.insert('Y');
        protag = ps;

    } else if (lower == "nanpa") {
        ProtagSpec ps;
        ps.entries.insert(0);
        protag = ps;

    } else if (lower == "nanpa2") {
        ProtagSpec ps;
        ps.entries.insert(0);
        protag = ps;

    } else if (lower == "dk") {
        engine = EngineType::AI1;
        // protag uses complex expression; store as-is for display
        // (+ (~ @ 36) (* 120 2)) and (+ M (* (~ J 0) 50))
        // these are expression-based protags, not simple IDs
        // for now, store a placeholder - these need expression evaluation
        ProtagSpec ps;
        protag = ps;

    } else if (lower == "dk2") {
        engine = EngineType::AI1;
        ProtagSpec ps;
        ps.entries.insert('G');
        protag = ps;

    } else if (lower == "dk3") {
        ProtagSpec ps;
        ps.entries.insert(26);
        protag = ps;

    } else if (lower == "elle") {
        ProtagSpec ps;
        ps.entries.insert('Z');
        protag = ps;

    } else if (lower == "isaku") {
        dict_base = 0xD0;
        extra_op = true;

    } else if (lower == "jack") {
        dict_base = 0xD0;

    } else if (lower == "jan") {
        ProtagSpec ps;
        ps.entries.insert(51);
        protag = ps;

    } else if (lower == "kakyu") {
        dict_base = 0xD0;
        ProtagSpec ps;
        ps.entries.insert(3);
        protag = ps;

    } else if (lower == "yuno") {
        dict_base = 0xD0;
        extra_op = true;
        ProtagSpec ps;
        ps.entries.insert(0);
        protag = ps;

    } else if (lower == "mobius") {
        dict_base = 0xD0;

    } else if (lower == "pinky") {
        engine = EngineType::AI1;
        ProtagSpec ps;
        ps.entries.insert('N');
        protag = ps;

    } else if (lower == "raygun") {
        engine = EngineType::AI1;

    } else if (lower == "ww") {
        ProtagSpec ps;
        ps.entries.insert(0);
        protag = ps;
    
    } else if (lower == "xgirl") {
        engine = EngineType::ADV;
        extra_op = true;

    } else {
        throw std::runtime_error("unknown preset: " + name);
    }
}

void Config::set_engine(const std::string& name) {
    std::string upper = name;
    std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);

    if (upper == "AI5" || upper == "AI4") {
        engine = EngineType::AI5;
    } else if (upper == "AI1" || upper == "AI2") {
        engine = EngineType::AI1;
    } else if (upper == "AI5X") {
        engine = EngineType::AI5;
        dict_base = 0xD0;
        extra_op = true;
    } else if (upper == "ADV") {
        engine = EngineType::ADV;
    } else if (upper == "AI5WIN") {
        engine = EngineType::AI5WIN;
    } else if (upper == "GM" || upper == "GENERAL-MESSAGE" || upper == "GENERAL_MESSAGE") {
        engine = EngineType::GM;
    } else {
        throw std::runtime_error("unknown engine type: " + name);
    }
}

void Config::set_protag(const std::string& spec) {
    if (spec == "all") {
        ProtagSpec ps;
        ps.all = true;
        protag = ps;
        return;
    }

    if (spec == "none") {
        protag = std::nullopt;
        return;
    }

    // parse comma-separated list of proc IDs or variable names
    ProtagSpec ps;
    std::istringstream ss(spec);
    std::string token;

    while (std::getline(ss, token, ',')) {
        // trim whitespace
        token.erase(0, token.find_first_not_of(" \t"));
        token.erase(token.find_last_not_of(" \t") + 1);

        if (token.empty()) {
            continue;
        }

        // check if it's a single uppercase letter (variable name)
        if (token.size() == 1 && token[0] >= 'A' && token[0] <= 'Z') {
            ps.entries.insert(token[0]);
        } else {
            // try to parse as number
            try {
                int n = std::stoi(token);
                ps.entries.insert(n);
            } catch (...) {
                throw std::runtime_error("unsupported protag proc/call: " + token);
            }
        }
    }

    protag = ps;
}

bool Config::is_protag(int proc_id) const {
    if (!protag.has_value()) {
        return false;
    }
    return protag->matches(proc_id);
}

bool Config::is_protag(char var_name) const {
    if (!protag.has_value()) {
        return false;
    }
    return protag->matches(var_name);
}

std::vector<PresetInfo> get_presets() {
    return {
        {"angel",  "Angel Hearts"},
        {"aishi",  "Ai Shimai"},
        {"beyond", "Be-Yond: Kurodaishou ni Mirareteru"},
        {"cre",    "Crescent"},
        {"coc",    "Curse of Castle"},
        {"deja",   "De-Ja"},
        {"deja2",  "De-Ja 2"},
        {"nanpa",  "Doukyuusei"},
        {"nanpa2", "Doukyuusei 2"},
        {"dk",     "Dragon Knight"},
        {"dk2",    "Dragon Knight 2"},
        {"dk3",    "Dragon Knight 3"},
        {"dk4",    "Dragon Knight 4"},
        {"elle",   "ELLE"},
        {"fermion", "Fermion: Mirai kara no Houmonsha"},
        {"foxy",   "Foxy"},
        {"foxy2",  "Foxy 2"},
        {"isaku",  "Isaku"},
        {"jack",   "Jack"},
        {"jan",    "Jan Jaka Jan"},
        {"kakyu",  "Kakyuusei"},
        {"kawa",   "Kawarazaki-ke no Ichizoku"},
        {"yuno",   "Kono Yo no Hate de Koi o Utau Shoujo YU-NO"},
        {"metal",  "Metal Eye"},
        {"metal2", "Metal Eye 2"},
        {"mobius", "Mobius Roid"},
        {"nono",   "Nonomura Byouin no Hitobito"},
        {"pinky",  "Pinky Ponky 1/2/3"},
        {"pre",    "Premium"},
        {"pre2",   "Premium 2"},
        {"raygun", "RAY-GUN"},
        {"reira",  "Reira Slave Doll"},
        {"syan",   "Shangrlia"},
        {"syan2",  "Shangrlia 2"},
        {"ten",    "Tenshin Ranma"},
        {"shima",  "Ushinawareta Rakuen"},
        {"ww",     "Words Worth"},
        {"xgirl",  "X-Girl"},
    };
}
