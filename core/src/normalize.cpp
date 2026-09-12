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

#include "ast.hpp"
#include "normalized.hpp"
#include "normalized_format.hpp"
#include "operators.hpp"
#include "reader.hpp"

#include <algorithm>
#include <array>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace normalized {
namespace {
    struct located_token {
        token_kind kind;
        lexeme value;
    };

    std::vector<located_token> tokenize(const source_span& input) {
        reader cursor(input);
        std::vector<located_token> result;
        while (true) {
            token next;
            cursor.next_token(next);
            if (next.kind == token_kind::whitespace
                || next.kind == token_kind::comment)
                continue;
            if (next.kind == token_kind::special_character) {
                // Only adjacent source bytes form a compound operator.
                static constexpr auto compound
                    = std::to_array<std::string_view>(
                        { "<<=", ">>=", "++", "--", "+=", "-=", "*=",
                          "/=",  "%=",  "^=", "|=", "&=", "==", "!=",
                          "<=",  ">=",  "<<", ">>", "&&", "||" }
                    );
                const auto rest = input.owner()->text().substr(
                    static_cast<size_t>(next.pos.offset),
                    static_cast<size_t>(input.end().offset - next.pos.offset)
                );
                for (const auto op : compound) {
                    if (rest.starts_with(op)) {
                        next.word = op;
                        cursor.jump_to_position(input.owner()->position_at(
                            next.pos.offset
                            + static_cast<std::streamoff>(op.size())
                        ));
                        break;
                    }
                }
            }
            result.push_back(
                { next.kind,
                  { next.word, cursor.span(next.pos, cursor.get_position()) } }
            );
            if (next.kind == token_kind::eof)
                return result;
        }
    }

    source_span group_input(const group_node& group, bool expression_mode) {
        if (!group.span)
            throw std::invalid_argument("group has no source span");
        auto begin = group.span->begin().offset;
        auto end = group.span->end().offset;
        if (group.kind == group_kind::body || group.kind == group_kind::list
            || group.kind == group_kind::paren)
            begin = group.get_start().offset;
        const auto owner = group.span->owner();
        const auto tokens = tokenize(source_span(owner, begin, end));
        size_t depth = 0;
        size_t last = tokens.size() - 1; // exclude EOF
        for (size_t i = 0; i < last; ++i) {
            if (tokens[i].kind == token_kind::open_bracket)
                ++depth;
            else if (tokens[i].kind == token_kind::close_bracket) {
                if (depth != 0)
                    --depth;
                else {
                    if (i + 1 != last)
                        throw std::invalid_argument(
                            "group boundary is not terminal"
                        );
                    end = tokens[i].value.span.begin().offset;
                    --last;
                }
            }
        }
        if (expression_mode && last != 0) {
            const auto& terminal = tokens[last - 1];
            if (terminal.kind == token_kind::separator
                && ((group.kind == group_kind::command
                     && terminal.value.text == ";")
                    || (group.kind == group_kind::item
                        && terminal.value.text == ",")
                    || (group.kind == group_kind::key
                        && terminal.value.text == ":"))) {
                end = terminal.value.span.begin().offset;
            }
        }
        return source_span(owner, begin, end);
    }

    class parser {
    public:
        parser(source_span input, limits budget)
            : input_(std::move(input))
            , tokens_(tokenize(input_))
            , budget_(budget) { }

        expression_ptr complete_expression() {
            const auto result = parse_expression();
            if (!at_end())
                fail("unexpected token after expression");
            return result;
        }

        statement_ptr complete_program() {
            std::vector<statement_ptr> statements;
            while (!at_end())
                statements.push_back(parse_statement());
            return make_statement(
                input_.begin().offset, input_.end().offset,
                block { std::move(statements), true }
            );
        }

    private:
        source_span input_;
        std::vector<located_token> tokens_;
        limits budget_;
        size_t cursor_ = 0, depth_ = 0, nodes_ = 0;

        struct depth_guard {
            parser& owner;

            explicit depth_guard(parser& p)
                : owner(p) {
                if (owner.depth_ >= owner.budget_.depth)
                    owner.fail("normalization depth limit exceeded");
                ++owner.depth_;
            }

            ~depth_guard() { --owner.depth_; }
        };

        const located_token& current() const { return tokens_[cursor_]; }

        bool at_end() const { return current().kind == token_kind::eof; }

        bool at(std::string_view text) const {
            return current().kind != token_kind::string
                && current().value.text == text;
        }

        lexeme take() {
            const auto result = current().value;
            if (!at_end())
                ++cursor_;
            return result;
        }

        lexeme expect(std::string_view text) {
            if (!at(text))
                fail("expected '" + std::string(text) + "'");
            return take();
        }

        [[noreturn]] void fail(const std::string& message) const {
            const auto pos = current().value.span.begin();
            throw std::runtime_error(
                input_.owner()->name() + ":"
                + std::to_string(static_cast<long long>(pos.line) + 1) + ":"
                + std::to_string(static_cast<long long>(pos.column) + 1) + ": "
                + message
            );
        }

        source_span range(std::streamoff begin, std::streamoff end) const {
            return source_span(input_.owner(), begin, end);
        }

        template <class T>
        expression_ptr make(std::streamoff begin, std::streamoff end, T value) {
            if (nodes_ >= budget_.nodes)
                fail("normalization node limit exceeded");
            ++nodes_;
            expression node { range(begin, end), std::move(value) };
            auto height = [&](const auto& child) {
                node.height = std::max(node.height, child->height + 1);
            };
            for_each_child(node, height, height);
            if (node.height > budget_.depth)
                fail("normalization tree depth limit exceeded");
            return std::make_shared<const expression>(std::move(node));
        }

        template <class T>
        statement_ptr
        make_statement(std::streamoff begin, std::streamoff end, T value) {
            if (nodes_ >= budget_.nodes)
                fail("normalization node limit exceeded");
            ++nodes_;
            statement node { range(begin, end), std::move(value) };
            auto height = [&](const auto& child) {
                node.height = std::max(node.height, child->height + 1);
            };
            for_each_child(node, height, height);
            if (node.height > budget_.depth)
                fail("normalization tree depth limit exceeded");
            return std::make_shared<const statement>(std::move(node));
        }

        static bool is_identifier(const located_token& tok) {
            if (tok.kind != token_kind::keyword)
                return false;
            static constexpr auto reserved = std::to_array<std::string_view>(
                { "fu", "if", "elif", "else", "return", "for", "while", "try",
                  "catch", "finally", "goto", "break", "continue", "true",
                  "false", "null" }
            );
            return std::find(reserved.begin(), reserved.end(), tok.value.text)
                == reserved.end();
        }

        bool named_function_ahead() const {
            if (!is_identifier(current()) || cursor_ + 1 >= tokens_.size()
                || tokens_[cursor_ + 1].value.text != "(")
                return false;
            size_t index = cursor_ + 2;
            bool need_name = true;
            while (index < tokens_.size() && tokens_[index].value.text != ")") {
                if (need_name) {
                    if (!is_identifier(tokens_[index]))
                        return false;
                } else if (tokens_[index].value.text != ",")
                    return false;
                need_name = !need_name;
                ++index;
            }
            return index + 1 < tokens_.size()
                && tokens_[index + 1].value.text == "{"
                && tokens_[index + 1].kind == token_kind::open_bracket;
        }

        expression_ptr parse_function(lexeme introducer) {
            const auto open = expect("(");
            std::vector<lexeme> parameters;
            std::unordered_set<std::string> names;
            while (!at(")")) {
                if (!is_identifier(current()))
                    fail("expected parameter name");
                if (!names.insert(current().value.text).second)
                    fail("duplicate parameter name");
                parameters.push_back(take());
                if (!at(","))
                    break;
                take();
            }
            const auto close = expect(")");
            const auto body = parse_block();
            return make(
                introducer.span.begin().offset, body->span.end().offset,
                function { introducer, open, close, std::move(parameters),
                           body }
            );
        }

        expression_ptr expression_sequence() {
            const auto first = parse_expression();
            if (!at(","))
                return first;
            std::vector<expression_ptr> items { first };
            while (at(",")) {
                take();
                items.push_back(parse_expression());
            }
            return make(
                first->span.begin().offset, items.back()->span.end().offset,
                tuple { items }
            );
        }

        bool statement_end() const {
            return at_end() || at("}") || at(";") || at("elif") || at("else")
                || at("catch") || at("finally");
        }

        std::streamoff finish_statement(std::streamoff end) {
            if (at(";"))
                return take().span.end().offset;
            if (!statement_end())
                fail("expected statement separator");
            return end;
        }

        statement_ptr parse_block() {
            depth_guard guard(*this);
            const auto open = expect("{");
            std::vector<statement_ptr> body;
            while (!at("}")) {
                if (at_end())
                    fail("expected '}'");
                body.push_back(parse_statement());
            }
            const auto close = expect("}");
            return make_statement(
                open.span.begin().offset, close.span.end().offset,
                block { std::move(body), false }
            );
        }

        statement_ptr parse_body() {
            return at("{") ? parse_block() : parse_statement();
        }

        expression_ptr condition() {
            expect("(");
            const auto value = expression_sequence();
            expect(")");
            return value;
        }

        statement_ptr parse_branch(bool conditional_return = false) {
            depth_guard guard(*this);
            const auto keyword = take(); // if or elif
            const auto cond = condition();
            if (conditional_return && !at("{"))
                fail("conditional return requires a braced first branch");
            const auto yes = parse_body();
            statement_ptr no;
            if (at("elif"))
                no = parse_branch(conditional_return);
            else if (at("else")) {
                take();
                no = parse_body();
            }
            return make_statement(
                keyword.span.begin().offset, (no ? no : yes)->span.end().offset,
                branch { keyword, cond, yes, no }
            );
        }

        statement_ptr parse_statement() {
            depth_guard guard(*this);
            const auto begin = current().value.span.begin().offset;
            if (at(";")) {
                const auto separator = take();
                return make_statement(
                    begin, separator.span.end().offset, empty_statement {}
                );
            }
            // String keys cannot be labels. Braces otherwise denote a block in
            // statement context, and always denote an object in expression
            // context.
            if (at("{")
                && !(
                    cursor_ + 2 < tokens_.size()
                    && tokens_[cursor_ + 1].kind == token_kind::string
                    && tokens_[cursor_ + 2].value.text == ":"
                ))
                return parse_block();
            if (at("if"))
                return parse_branch();
            if (at("while")) {
                const auto keyword = take();
                const auto cond = condition();
                const auto body = parse_body();
                return make_statement(
                    begin, body->span.end().offset,
                    while_loop { keyword, cond, body }
                );
            }
            if (at("for")) {
                const auto keyword = take();
                expect("(");
                expression_ptr setup, cond, step;
                if (!at(";"))
                    setup = expression_sequence();
                expect(";");
                if (!at(";"))
                    cond = expression_sequence();
                expect(";");
                if (!at(")"))
                    step = expression_sequence();
                expect(")");
                const auto body = parse_body();
                return make_statement(
                    begin, body->span.end().offset,
                    for_loop { keyword, setup, cond, step, body }
                );
            }
            if (at("try")) {
                const auto keyword = take();
                const auto body = parse_body();
                std::vector<catch_clause> handlers;
                statement_ptr finalizer;
                while (at("catch")) {
                    const auto catch_keyword = take();
                    expect("(");
                    if (!is_identifier(current()))
                        fail("expected catch binding");
                    const auto binding = take();
                    expect(")");
                    handlers.push_back(
                        { catch_keyword, binding, parse_body() }
                    );
                }
                if (at("finally")) {
                    take();
                    finalizer = parse_body();
                }
                if (handlers.empty() && !finalizer)
                    fail("try requires catch or finally");
                const auto end = finalizer
                    ? finalizer->span.end().offset
                    : handlers.back().body->span.end().offset;
                return make_statement(
                    begin, end,
                    try_statement { keyword, body, std::move(handlers),
                                    finalizer }
                );
            }
            if (at("return") || at("goto") || at("break") || at("continue")) {
                const auto keyword = take();
                expression_ptr value;
                statement_ptr conditional;
                std::optional<lexeme> target;
                auto end = keyword.span.end().offset;
                if (keyword.text == "goto") {
                    if (!is_identifier(current()))
                        fail("expected goto label");
                    target = take();
                    end = target->span.end().offset;
                } else if (keyword.text == "return" && !statement_end()) {
                    if (at("if")) {
                        conditional = parse_branch(true);
                        end = conditional->span.end().offset;
                    } else {
                        value = expression_sequence();
                        end = value->span.end().offset;
                    }
                }
                end = finish_statement(end);
                return make_statement(
                    begin, end, jump { keyword, value, conditional, target }
                );
            }
            if (is_identifier(current()) && cursor_ + 1 < tokens_.size()
                && tokens_[cursor_ + 1].kind == token_kind::separator
                && tokens_[cursor_ + 1].value.text == ":") {
                const auto name = take();
                const auto colon = take();
                return make_statement(
                    begin, colon.span.end().offset, label { name }
                );
            }
            if (named_function_ahead()) {
                const auto name = take();
                const auto function = parse_function(name);
                const auto end = at(";") ? take().span.end().offset
                                         : function->span.end().offset;
                return make_statement(
                    begin, end, definition { name, function }
                );
            }
            const auto value = expression_sequence();
            const auto end = finish_statement(value->span.end().offset);
            return make_statement(begin, end, expression_statement { value });
        }

        expression_ptr parse_expression(int minimum = 0) {
            depth_guard guard(*this);
            auto left = prefix();
            while (true) {
                if (at("?") && minimum <= 2) {
                    const auto question = take();
                    const auto yes = parse_expression();
                    const auto colon = expect(":");
                    const auto no = parse_expression(2);
                    left = make(
                        left->span.begin().offset, no->span.end().offset,
                        ternary { question, colon, left, yes, no }
                    );
                    continue;
                }
                const auto* op = binary_operator(current().value.text);
                if (current().kind != token_kind::special_character || !op
                    || op->precedence < minimum)
                    break;
                const auto spelling = take();
                const auto right = parse_expression(
                    op->precedence + (op->right_associative ? 0 : 1)
                );
                left = make(
                    left->span.begin().offset, right->span.end().offset,
                    binary { spelling, left, right }
                );
            }
            return left;
        }

        expression_ptr prefix() {
            if (at("+") || at("-") || at("!") || at("~") || at("++")
                || at("--")) {
                const auto op = take();
                const auto operand = parse_expression(13);
                return make(
                    op.span.begin().offset, operand->span.end().offset,
                    unary { op, operand, true }
                );
            }
            return postfix(primary());
        }

        std::vector<expression_ptr> elements(std::string_view close) {
            std::vector<expression_ptr> values;
            if (at(close))
                return values;
            while (true) {
                values.push_back(parse_expression());
                if (!at(","))
                    return values;
                take();
                if (at(close))
                    return values; // trailing comma
            }
        }

        expression_ptr primary() {
            const auto first = current();
            const auto begin = first.value.span.begin().offset;
            if (first.kind == token_kind::special_character
                && (at("$") || at("@"))) {
                take();
                return make(
                    begin, first.value.span.end().offset,
                    context_reference { first.value.text == "$"
                                            ? reference_base::root
                                            : reference_base::current,
                                        first.value }
                );
            }
            if (at("fu"))
                return parse_function(take());
            if (first.kind == token_kind::integer
                || first.kind == token_kind::floating
                || first.kind == token_kind::string || at("true") || at("false")
                || at("null")) {
                auto kind = literal_kind::integer;
                if (first.kind == token_kind::floating)
                    kind = literal_kind::decimal;
                else if (first.kind == token_kind::string)
                    kind = literal_kind::string;
                else if (at("null"))
                    kind = literal_kind::null;
                else if (at("true") || at("false"))
                    kind = literal_kind::boolean;
                take();
                return make(
                    begin, first.value.span.end().offset,
                    literal { kind, first.value }
                );
            }
            if (first.kind == token_kind::keyword) {
                static constexpr auto reserved
                    = std::to_array<std::string_view>(
                        { "fu", "return", "if", "elif", "else", "for", "while",
                          "try", "catch", "finally", "goto", "break",
                          "continue" }
                    );
                if (std::find(
                        reserved.begin(), reserved.end(), first.value.text
                    )
                    != reserved.end())
                    fail("expected expression");
                take();
                return make(
                    begin, first.value.span.end().offset,
                    identifier { first.value }
                );
            }
            if (at("[")) {
                take();
                auto values = elements("]");
                const auto close = expect("]");
                return make(
                    begin, close.span.end().offset, list { std::move(values) }
                );
            }
            if (at("(")) {
                take();
                if (at(")")) {
                    const auto close = take();
                    return make(begin, close.span.end().offset, tuple {});
                }
                const auto first_value = parse_expression();
                if (!at(",")) {
                    const auto close = expect(")");
                    return make(
                        begin, close.span.end().offset, first_value->value
                    );
                }
                take();
                auto rest = elements(")");
                rest.insert(rest.begin(), first_value);
                const auto close = expect(")");
                return make(
                    begin, close.span.end().offset, tuple { std::move(rest) }
                );
            }
            if (at("{")) {
                take();
                std::vector<object_entry> entries;
                while (!at("}")) {
                    if (current().kind != token_kind::keyword
                        && current().kind != token_kind::string)
                        fail("expected constant object key");
                    const auto key = take();
                    const auto colon = expect(":");
                    entries.push_back({ key, colon, parse_expression() });
                    if (!at(","))
                        break;
                    take();
                }
                const auto close = expect("}");
                return make(
                    begin, close.span.end().offset,
                    object { std::move(entries) }
                );
            }
            fail("expected expression");
        }

        expression_ptr postfix(expression_ptr base) {
            while (true) {
                const auto begin = base->span.begin().offset;
                if (at(".")) {
                    const auto dot = take();
                    if (at("(")) {
                        take();
                        std::vector<lexeme> names;
                        if (at(")"))
                            fail("expected member name");
                        while (true) {
                            if (current().kind != token_kind::keyword
                                && current().kind != token_kind::string)
                                fail("expected member name");
                            names.push_back(take());
                            if (!at(","))
                                break;
                            take();
                        }
                        const auto close = expect(")");
                        base = make(
                            begin, close.span.end().offset,
                            member_selection { base, dot, std::move(names) }
                        );
                    } else {
                        if (current().kind != token_kind::keyword)
                            fail("expected member name");
                        const auto name = take();
                        base = make(
                            begin, name.span.end().offset,
                            member { base, dot, name }
                        );
                    }
                } else if (at("(")) {
                    const auto open = take();
                    auto args = elements(")");
                    const auto close = expect(")");
                    base = make(
                        begin, close.span.end().offset,
                        call { base, std::move(args), open, close }
                    );
                } else if (at("[")) {
                    const auto open = take();
                    expression_ptr start, stop, step;
                    if (!at(":"))
                        start = parse_expression();
                    if (!at(":")) {
                        const auto close = expect("]");
                        base = make(
                            begin, close.span.end().offset,
                            index { base, start, open, close }
                        );
                        continue;
                    }
                    std::vector<lexeme> colons { take() };
                    if (!at(":") && !at("]"))
                        stop = parse_expression();
                    if (at(":")) {
                        colons.push_back(take());
                        if (!at("]"))
                            step = parse_expression();
                    }
                    const auto close = expect("]");
                    base = make(
                        begin, close.span.end().offset,
                        slice { base, start, stop, step, open, close,
                                std::move(colons) }
                    );
                } else if (at("{")) {
                    const auto open = take();
                    auto keys = elements("}");
                    const auto close = expect("}");
                    if (keys.empty())
                        fail("selection requires at least one key");
                    base = make(
                        begin, close.span.end().offset,
                        selection { base, std::move(keys), open, close }
                    );
                } else if (at("++") || at("--")) {
                    const auto op = take();
                    base = make(
                        begin, op.span.end().offset, unary { op, base, false }
                    );
                } else
                    return base;
            }
        }
    };
}

expression_ptr normalize_expression(source_span input, limits budget) {
    return parser(std::move(input), budget).complete_expression();
}

statement_ptr normalize_program(source_span input, limits budget) {
    return parser(std::move(input), budget).complete_program();
}

expression_ptr normalize_expression(const group_node& group, limits budget) {
    return normalize_expression(group_input(group, true), budget);
}

statement_ptr normalize_program(const group_node& group, limits budget) {
    return normalize_program(group_input(group, false), budget);
}

void dump_tokens(std::ostream& output, source_span input) {
    for (const auto& token : tokenize(input)) {
        const auto begin = token.value.span.begin();
        output << begin.offset << ':' << token.value.span.end().offset << " <"
               << begin.line << ':' << begin.column << "> "
               << detail::quoted_text(token.value.text) << '\n';
    }
}

}
