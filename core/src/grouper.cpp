/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2025 Yaroslav Riabtsev <yaroslav.riabtsev@rwth-aachen.de>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "grouper.hpp"

#include "expression.hpp"
#include <algorithm>
#include <limits>

grouper::grouper(reader& r, const size_t limit)
    : src(r)
    , limit(limit) {
    if (limit < 2) {
        throw make_error("minimum limit is 2");
    }
}

group_ptr grouper::parse(const group_kind kind) {
    group_ptr group, result;
    if (kind == group_kind::body || kind == group_kind::list
        || kind == group_kind::paren) {
        group = std::make_shared<wrapped_node>();
        result = std::make_shared<wrapped_node>();
    } else {
        group = std::make_shared<group_node>();
        result = std::make_shared<group_node>();
    }
    group->limit = limit;
    group->kind = kind;
    result->limit = limit;
    result->kind = kind;
    parse_group(kind, group);
    // A halt is the trailing statement itself, not a container around it.
    // parse_group adds a container while consuming a closing delimiter/EOF.
    if (kind == group_kind::halt && group->size() == 1) {
        const auto trailing
            = std::dynamic_pointer_cast<group_node>(group->nodes.front());
        if (trailing && trailing->kind == group_kind::halt) {
            group = trailing;
        }
    }
    identify(group, result);
    return result;
}

group_ptr grouper::parse_chain() {
    auto raw = std::make_shared<group_node>();
    raw->limit = std::numeric_limits<size_t>::max();
    raw->kind = group_kind::file;
    parse_group(group_kind::file, raw, true);
    const auto result = std::make_shared<group_node>();
    // This temporary container must not squeeze the chain it reconstructs.
    result->limit = std::numeric_limits<size_t>::max();
    result->kind = group_kind::file;
    identify(raw, result);
    if (result->nodes.size() == 2 && result->nodes.back()->empty()) {
        result->pop_back();
    }
    if (result->nodes.size() != 1) {
        throw make_error("expected one bounded command chain", result);
    }
    const auto chain = std::dynamic_pointer_cast<group_node>(result->nodes[0]);
    if (!chain || !chain->is_chain) {
        throw make_error("expected a merged command chain", result);
    }
    return chain;
}

void grouper::parse_group(
    const group_kind kind, group_ptr& group, const bool bounded_sequence
) {
    const auto begin = src.get_position();
    auto top = std::make_shared<group_node>();
    top->limit = limit;
    top->span = src.span(begin, begin);
    while (true) {
        peek();
        if (current.kind == token_kind::separator) {
            if (append_command(group, top, kind)) {
                group->span = src.span(begin, src.get_position());
                return;
            }
        } else if (current.kind == token_kind::open_bracket) {
            append_wrapped(top);
        } else if (
            current.kind == token_kind::close_bracket
            || current.kind == token_kind::eof
        ) {
            close_wrapped(group, top, kind, bounded_sequence);
            group->span = src.span(begin, src.get_position());
            return;
        } else {
            auto tk = std::make_shared<token_node>();
            tk->value = current;
            append(top, tk);
        }
    }
}

void grouper::append(
    const group_ptr& parent, const ast_node_ptr& node,
    const std::source_location& location
) const {
    try {
        parent->append(node);
    } catch (const std::runtime_error& e) {
        std::stringstream msg;
        msg << "failed to append node: \n";
        node->dump(msg, "", true, false);
        msg << e.what();
        throw make_error(msg.str(), parent, location);
    }
}

void grouper::peek() {
    if (reuse) {
        reuse = false;
        return;
    }
    do {
        pos = src.get_position();
        src.next_token(current);
    } while (current.kind == token_kind::whitespace
             || current.kind == token_kind::comment);
}

group_ptr grouper::identify_subgroup(const group_ptr& group) const {
    if (std::dynamic_pointer_cast<placeholder_node>(group)) {
        return group;
    }
    group_ptr inode;
    const auto kind = group->kind;
    if (kind == group_kind::body || kind == group_kind::list
        || kind == group_kind::paren) {
        inode = std::make_shared<wrapped_node>();
    } else {
        inode = std::make_shared<group_node>();
    }
    inode->limit = limit;
    inode->kind = kind;
    identify(group, inode);
    return inode;
}

bool grouper::is_secondary_keyword(const std::string& kw) {
    return kw == "else" || kw == "elif" || kw == "catch" || kw == "finally";
}

std::string grouper::keyword_from_node(const ast_node_ptr& node) {
    if (const auto ctrl = std::dynamic_pointer_cast<control_node>(node)) {
        return ctrl->value.word;
    }
    if (const auto cond = std::dynamic_pointer_cast<condition_node>(node)) {
        return cond->value.word;
    }
    return {};
}

group_ptr grouper::fetch_previous_command(
    const group_ptr& result, const std::string& kw, const group_ptr& inode
) const {
    if (result->empty()) {
        throw make_error("orphan secondary keyword: " + kw, inode);
    }
    const auto prev
        = std::dynamic_pointer_cast<group_node>(result->nodes.back());
    if (!prev || prev->nodes.empty() || prev->kind != group_kind::command) {
        throw make_error("invalid predecessor for keyword: " + kw, inode);
    }
    return prev;
}

std::string grouper::fetch_previous_keyword(
    const group_ptr& prev, const std::string& kw, const group_ptr& inode
) const {
    const auto last = prev->nodes.back();
    if (const auto ctrl = std::dynamic_pointer_cast<control_node>(last)) {
        return ctrl->value.word;
    }
    if (const auto cond = std::dynamic_pointer_cast<condition_node>(last)) {
        return cond->value.word;
    }
    throw make_error("invalid predecessor for keyword: " + kw, inode);
}

void grouper::validate_chain(
    const std::string& prev_kw, const std::string& kw, const group_ptr& inode
) const {
    bool allowed = false;
    if (kw == "else" || kw == "elif") {
        allowed = (prev_kw == "if" || prev_kw == "elif");
    } else if (kw == "catch" || kw == "finally") {
        allowed = (prev_kw == "try" || prev_kw == "catch");
    }
    if (!allowed) {
        throw make_error(
            "unexpected keyword order: " + prev_kw + " before " + kw, inode
        );
    }
}

bool grouper::handle_chain(
    const group_ptr& result, const group_ptr& inode
) const {
    const auto first = inode->nodes.front();
    const auto kw = keyword_from_node(first);
    if (!is_secondary_keyword(kw)) {
        return false;
    }
    const auto prev = fetch_previous_command(result, kw, inode);
    const auto prev_kw = fetch_previous_keyword(prev, kw, inode);
    validate_chain(prev_kw, kw, inode);
    result->pop_back();
    if (prev->span && inode->span) {
        prev->span = src.span(prev->span->begin(), inode->span->end());
    }
    prev->is_chain = true;
    for (auto& ch : inode->nodes) {
        append(prev, ch);
    }
    append(result, prev);
    return true;
}

bool grouper::append_group(
    const group_ptr& result, const ast_node_ptr& node, bool& wait_for_condition,
    bool& wait_for_body, const group_kind kind
) const {
    if (!result->empty()) {
        const auto top = result->nodes.back();
        result->pop_back();
        if (const auto cond = std::dynamic_pointer_cast<condition_node>(top);
            cond && kind == group_kind::paren) {
            cond->set_paren(node);
            append(result, cond);
            wait_for_condition = false;
            wait_for_body = true;
            return true;
        }
        if (const auto ctrl = std::dynamic_pointer_cast<control_node>(top);
            ctrl && kind == group_kind::body) {
            wait_for_body = false;
            ctrl->set_body(node);
            append(result, ctrl);
            return true;
        }
        if (const auto callexp = std::dynamic_pointer_cast<callexp_node>(top)) {
            if (kind == group_kind::body) {
                const auto fundecl = std::make_shared<fundecl_node>(callexp);
                fundecl->set_body(node);
                append(result, fundecl);
                return true;
            }
            if (kind == group_kind::paren) {
                const auto icall = std::make_shared<imcallexp_node>(top);
                icall->set_paren(node);
                append(result, icall);
                return true;
            }
        }
        const auto tok = std::dynamic_pointer_cast<token_node>(top);
        if (tok && tok->value.kind == token_kind::keyword
            && kind == group_kind::paren) {
            const auto callexp = std::make_shared<callexp_node>(tok->value);
            callexp->set_paren(node);
            append(result, callexp);
            return true;
        }
        if (kind == group_kind::paren && !tok) {
            const auto icall = std::make_shared<imcallexp_node>(top);
            icall->set_paren(node);
            append(result, icall);
            return true;
        }
        append(result, top);
    }
    return false;
}

void grouper::identify(const group_ptr& group, const group_ptr& result) const {
    result->span = group->span;
    result->is_chain = group->is_chain;
    if (const auto wrapped = std::dynamic_pointer_cast<wrapped_node>(group)) {
        if (const auto output
            = std::dynamic_pointer_cast<wrapped_node>(result)) {
            output->start = wrapped->start;
        }
    }
    bool wait_for_condition = false;
    bool wait_for_body = false;

    for (size_t i = 0; i < group->nodes.size(); ++i) {
        auto node = group->nodes[i];
        bool is_group = false;
        group_kind kind {};

        if (auto sub = std::dynamic_pointer_cast<group_node>(node)) {
            kind = sub->kind;
            auto inode = identify_subgroup(sub);
            node = inode;
            is_group = true;

            if ((kind == group_kind::halt || kind == group_kind::command)
                && !inode->nodes.empty()) {
                if (handle_chain(result, inode)) {
                    continue;
                }
            }
        }
        if (wait_for_condition && (!is_group || kind != group_kind::paren)) {
            throw make_error("expected condition after control keyword");
        }
        if (is_group) {
            if (append_group(
                    result, node, wait_for_condition, wait_for_body, kind
                )) {
                continue;
            }
        }
        if (wait_for_body && !is_group) {
            const auto tail = std::make_shared<group_node>();
            tail->limit = limit;
            if (group->span) {
                tail->span = src.span(node->get_start(), group->span->end());
            }
            for (; i < group->nodes.size(); ++i) {
                append(tail, group->nodes[i]);
            }
            const auto body = std::make_shared<group_node>();
            body->limit = limit;
            identify(tail, body);

            const auto top = result->nodes.back();
            result->pop_back();
            if (const auto ctrl
                = std::dynamic_pointer_cast<control_node>(top)) {
                ctrl->set_body(body);
                append(result, ctrl);
            } else if (
                auto callexp = std::dynamic_pointer_cast<callexp_node>(top)
            ) {
                const auto fundecl = std::make_shared<fundecl_node>(callexp);
                fundecl->set_body(body);
                append(result, fundecl);
            }
            wait_for_body = false;
            continue;
        }
        if (const auto tok = std::dynamic_pointer_cast<token_node>(node)) {
            if (tok->value.kind == token_kind::keyword) {
                const auto& w = tok->value.word;
                if (w == "if" || w == "elif" || w == "while" || w == "for"
                    || w == "catch") {
                    wait_for_condition = true;
                    auto cond = std::make_shared<condition_node>(tok->value);
                    append(result, cond);
                    continue;
                }
                if (w == "else" || w == "try" || w == "finally") {
                    wait_for_body = true;
                    auto ctrl = std::make_shared<control_node>(tok->value);
                    append(result, ctrl);
                    continue;
                }
                if (w == "return" || w == "continue" || w == "break"
                    || w == "goto") {
                    auto jmp = std::make_shared<jump_node>(tok->value);
                    append(result, jmp);
                    wait_for_body = (w != "continue" && w != "break");
                    continue;
                }
            }
        }
        append(result, node);
    }
    try {
        parse_arithmetic(result);
    } catch (const std::runtime_error& e) {
        throw make_error(e.what(), result);
    }
}

bool grouper::append_command(
    group_ptr& group, group_ptr& top, const group_kind kind
) const {
    top->span = src.span(top->span->begin(), src.get_position());
    if (current.word == ":") {
        top->kind = group_kind::key;
    } else if (current.word == ",") {
        top->kind = group_kind::item;
    } else if (current.word == ";") {
        top->kind = group_kind::command;
    } else {
        throw make_error("unexpected separator: " + current.word, top);
    }
    if (top->kind == kind) {
        if (group->empty()) {
            group = top;
            return true;
        }
        append(group, top);
        throw make_error(
            "wrong group kind. expected: " + std::string(group_kind_name(kind))
                + ", got: " + group_kind_name(group->kind),
            group
        );
    }
    append(group, top);
    top = std::make_shared<group_node>();
    top->limit = limit;
    top->span = src.span(src.get_position(), src.get_position());
    return false;
}

void grouper::append_wrapped(const group_ptr& top) {
    group_kind sub_kind;
    if (current.word == "{") {
        sub_kind = group_kind::body;
    } else if (current.word == "[") {
        sub_kind = group_kind::list;
    } else if (current.word == "(") {
        sub_kind = group_kind::paren;
    } else {
        throw make_error("unexpected open bracket: " + current.word, top);
    }
    const auto wn = std::make_shared<wrapped_node>();
    wn->start = pos;
    wn->limit = limit;
    wn->kind = sub_kind;
    auto gr = std::dynamic_pointer_cast<group_node>(wn);
    parse_group(sub_kind, gr);
    append(top, gr);
}

void grouper::close_wrapped(
    const group_ptr& group, group_ptr& top, const group_kind kind,
    const bool bounded_sequence
) {
    top->span = src.span(top->span->begin(), src.get_position());
    append(group, top);
    top = std::make_shared<group_node>();
    top->limit = limit;
    if (current.kind == token_kind::eof) {
        group->kind = group_kind::file;
    } else if (current.word == "}") {
        group->kind = group_kind::body;
    } else if (current.word == "]") {
        group->kind = group_kind::list;
    } else if (current.word == ")") {
        group->kind = group_kind::paren;
    } else {
        throw make_error("unexpected close bracket: " + current.word, group);
    }
    if (bounded_sequence && src.get_position() == src.input().end()) {
        group->kind = kind;
        return;
    }
    if (kind == group_kind::halt) {
        reuse = true;
        return;
    }
    if (group->kind == kind) {
        return;
    }
    throw make_error(
        "wrong group kind. expected: " + std::string(group_kind_name(kind))
            + ", got: " + group_kind_name(group->kind),
        group
    );
}

std::runtime_error grouper::make_error(
    const std::string& message, const group_ptr& context,
    const std::source_location& location
) const {
    std::ostringstream oss;
    oss << "[Grouper-Error] " << message << ". " << std::endl;
    if (context) {
        oss << "during parsing of group:" << std::endl;
        context->dump(oss, "\t", true, false);
    }
    oss << "in file: " << location.file_name() << '(' << location.line() << ':'
        << location.column() << ") `" << location.function_name() << "`"
        << std::endl;
    try {
        src.interrupt();
    } catch (const std::runtime_error& e) {
        oss << e.what();
    }
    return std::runtime_error(oss.str());
}

void grouper::parse_arithmetic(const group_ptr& group) const {
    if (group->kind == group_kind::key && group->size() == 2) {
        const auto left_g
            = std::dynamic_pointer_cast<group_node>(group->nodes[0]);
        const auto right_g
            = std::dynamic_pointer_cast<group_node>(group->nodes[1]);
        bool has_q = false;
        if (left_g) {
            for (auto& ch : left_g->nodes) {
                if (const auto tn = std::dynamic_pointer_cast<token_node>(ch);
                    tn && tn->value.word == "?") {
                    has_q = true;
                    break;
                }
            }
        }
        if (has_q) {
            std::vector<ast_node_ptr> combined;
            if (left_g) {
                combined.insert(
                    combined.end(), left_g->nodes.begin(), left_g->nodes.end()
                );
            } else {
                combined.push_back(group->nodes[0]);
            }
            auto colon_tn = std::make_shared<token_node>();
            colon_tn->value.kind = token_kind::separator;
            colon_tn->value.word = ":";
            colon_tn->value.pos = group->nodes[1]->get_start();
            combined.push_back(colon_tn);
            if (right_g) {
                combined.insert(
                    combined.end(), right_g->nodes.begin(), right_g->nodes.end()
                );
            } else {
                combined.push_back(group->nodes[1]);
            }
            auto items = expression::make_items(combined);
            size_t idx = 0;
            auto expr = expression::parse_expression(items, idx, 0);
            if (idx == items.size()) {
                group->nodes.clear();
                group->weights = {};
                group->fixed_size = 1;
                group->full_size = 1;
                group->append(expr);
            }
            return;
        }
    }

    if (group->kind == group_kind::command || group->kind == group_kind::item
        || group->kind == group_kind::paren
        || group->kind == group_kind::halt) {
        if (group->nodes.empty()) {
            return;
        }
        auto items = expression::make_items(group->nodes);
        size_t idx = 0;
        const auto expr = expression::parse_expression(items, idx, 0);
        if (idx == items.size()) {
            group->nodes.clear();
            group->weights = {};
            group->fixed_size = 1;
            group->full_size = 1;
            append(group, expr);
        }
    }
}
