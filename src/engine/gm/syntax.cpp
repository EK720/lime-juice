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

#include "syntax.h"

#include <cstdint>
#include <iterator>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace gm {

namespace {

void count_local_addresses(const AstNode& node,
                           std::unordered_map<int32_t, size_t>& counts) {
    if (node.is_list("local-address") && node.children.size() == 1 &&
        node.children[0].is_integer()) {
        counts[node.children[0].int_val]++;
    }

    for (const auto& child : node.children) {
        count_local_addresses(child, counts);
    }
}

bool label_id(const AstNode& node, int32_t& id) {
    if (!node.is_list("label") || node.children.size() != 1 ||
        !node.children[0].is_integer()) {
        return false;
    }

    id = node.children[0].int_val;
    return true;
}

bool continuation_call(const std::vector<AstNode>& nodes, size_t index,
                       int32_t& continuation) {
    if (index + 2 >= nodes.size()) {
        return false;
    }

    const auto& call = nodes[index];
    if (!call.is_list("gosub-if-save") || call.children.size() != 3 ||
        !call.children[0].is_list("local-address") ||
        call.children[0].children.size() != 1 ||
        !call.children[0].children[0].is_integer() ||
        !nodes[index + 1].is_list("end") ||
        !label_id(nodes[index + 2], continuation)) {
        return false;
    }

    return call.children[0].children[0].int_val == continuation;
}

bool while_start(const std::vector<AstNode>& nodes, size_t index,
                 const std::unordered_map<int32_t, size_t>& local_uses,
                 size_t& label_index) {
    const auto& branch = nodes[index];
    if (!branch.is_list("if-frame") || branch.children.size() != 2 ||
        !branch.children[1].is_list("local-address") ||
        branch.children[1].children.size() != 1 ||
        !branch.children[1].children[0].is_integer()) {
        return false;
    }

    int32_t target = branch.children[1].children[0].int_val;
    auto uses = local_uses.find(target);
    if (uses == local_uses.end() || uses->second != 1) {
        return false;
    }

    for (size_t i = index + 2; i < nodes.size(); i++) {
        int32_t candidate = 0;
        if (!label_id(nodes[i], candidate) || candidate != target) {
            continue;
        }

        if (!nodes[i - 1].is_list("while-continue")) {
            return false;
        }
        label_index = i;
        return true;
    }

    return false;
}

void collect_label_ids(const AstNode& node,
                       std::unordered_set<int32_t>& used) {
    int32_t id = 0;
    bool found = label_id(node, id);
    if (!found && node.is_list("local-address") && node.children.size() == 1 &&
        node.children[0].is_integer()) {
        id = node.children[0].int_val;
        found = true;
    }
    if (found) {
        if (id >= 0 && id <= 0xffff) used.insert(id);
    }

    for (const auto& child : node.children) collect_label_ids(child, used);
}

class LabelAllocator {
public:
    explicit LabelAllocator(const std::vector<AstNode>& nodes) {
        for (const auto& node : nodes) collect_label_ids(node, used_);
    }

    int32_t next() {
        while (candidate_ >= 0 && used_.count(candidate_) != 0) candidate_--;
        if (candidate_ < 0) {
            throw std::runtime_error("gm: no label identifiers available");
        }
        int32_t result = candidate_--;
        used_.insert(result);
        return result;
    }

private:
    std::unordered_set<int32_t> used_;
    int32_t candidate_ = 0xffff;
};

void lower_node(const AstNode& node, std::vector<AstNode>& result,
                LabelAllocator& labels) {
    if (!node.is_list("while")) {
        result.push_back(node);
        return;
    }

    if (node.children.empty()) {
        throw std::runtime_error("gm: while requires a condition");
    }

    int32_t exit = labels.next();
    result.push_back(AstNode::make_list("if-frame", {
        node.children[0],
        AstNode::make_list("local-address", {AstNode::make_integer(exit)})
    }));
    for (size_t i = 1; i < node.children.size(); i++) {
        lower_node(node.children[i], result, labels);
    }
    result.push_back(AstNode::make_list("while-continue"));
    result.push_back(AstNode::make_list("label", {AstNode::make_integer(exit)}));
}

std::vector<AstNode> fuse_syntax_with_uses(
    std::vector<AstNode> nodes,
    const std::unordered_map<int32_t, size_t>& local_uses) {
    std::vector<AstNode> result;
    result.reserve(nodes.size());

    for (size_t i = 0; i < nodes.size();) {
        int32_t continuation = 0;
        if (continuation_call(nodes, i, continuation)) {
            auto& encoded = nodes[i];
            std::vector<AstNode> children;
            children.push_back(std::move(encoded.children[1]));
            if (!encoded.children[2].is_symbol("default")) {
                children.push_back(std::move(encoded.children[2]));
            }
            result.push_back(AstNode::make_list("call", std::move(children)));

            if (local_uses.at(continuation) > 1) {
                result.push_back(std::move(nodes[i + 2]));
            }
            i += 3;
            continue;
        }

        size_t label_index = 0;
        if (while_start(nodes, i, local_uses, label_index)) {
            std::vector<AstNode> body;
            body.reserve(label_index - i - 2);
            for (size_t j = i + 1; j + 1 < label_index; j++) {
                body.push_back(std::move(nodes[j]));
            }
            body = fuse_syntax_with_uses(std::move(body), local_uses);

            std::vector<AstNode> children;
            children.push_back(std::move(nodes[i].children[0]));
            children.insert(children.end(),
                            std::make_move_iterator(body.begin()),
                            std::make_move_iterator(body.end()));
            result.push_back(AstNode::make_list("while", std::move(children)));
            i = label_index + 1;
            continue;
        }

        result.push_back(std::move(nodes[i++]));
    }

    return result;
}

} // namespace

std::vector<AstNode> fuse_syntax(std::vector<AstNode> nodes) {
    std::unordered_map<int32_t, size_t> local_uses;
    for (const auto& node : nodes) {
        count_local_addresses(node, local_uses);
    }

    return fuse_syntax_with_uses(std::move(nodes), local_uses);
}

std::vector<AstNode> lower_syntax(const std::vector<AstNode>& nodes) {
    LabelAllocator labels(nodes);
    std::vector<AstNode> result;
    result.reserve(nodes.size());
    for (const auto& node : nodes) lower_node(node, result, labels);
    return result;
}

} // namespace gm
