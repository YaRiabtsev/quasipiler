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

#ifndef NORMALIZED_HPP
#define NORMALIZED_HPP

#include "exact.hpp"
#include "source.hpp"
#include <functional>
#include <iosfwd>
#include <optional>
#include <variant>

struct group_node;

namespace normalized {
struct expression;
struct statement;
using expression_ptr = std::shared_ptr<const expression>;
using statement_ptr = std::shared_ptr<const statement>;

/// Text decoded by the lexer, with the exact spelling retained in span.
struct lexeme {
    std::string text;
    source_span span;
};
enum class literal_kind { integer, decimal, string, boolean, null };

struct literal {
    literal_kind kind;
    lexeme token;
};

// Reducer output, distinct from source-spelled literals. Null is monostate.
using scalar = std::variant<std::monostate, bool, std::string, exact::number>;

struct constant {
    scalar value;
};

struct identifier {
    lexeme name;
};

enum class reference_base { root, current };

struct context_reference {
    reference_base base;
    lexeme token;
};

struct unary {
    lexeme op;
    expression_ptr operand;
    bool prefix;
};

struct binary {
    lexeme op;
    expression_ptr left, right;
};

struct ternary {
    lexeme question, colon;
    expression_ptr condition, yes, no;
};

struct list {
    std::vector<expression_ptr> elements;
};

struct tuple {
    std::vector<expression_ptr> elements;
};

struct object_entry {
    lexeme key, colon;
    expression_ptr value;
};

struct object {
    std::vector<object_entry> entries;
};

struct member {
    expression_ptr base;
    lexeme dot, name;
};

struct member_selection {
    expression_ptr base;
    lexeme dot;
    std::vector<lexeme> names;
};

struct index {
    expression_ptr base, subscript;
    lexeme open, close;
};

struct slice {
    expression_ptr base, start, stop, step; // null means omitted bound
    lexeme open, close;
    std::vector<lexeme> colons;
};

struct selection {
    expression_ptr base;
    std::vector<expression_ptr> keys;
    lexeme open, close;
};

struct call {
    expression_ptr callee;
    std::vector<expression_ptr> arguments;
    lexeme open, close;
};

struct function {
    lexeme introducer, open, close;
    std::vector<lexeme> parameters;
    statement_ptr body;
};

using expression_value = std::variant<
    literal, identifier, unary, binary, ternary, list, tuple, object, member,
    member_selection, index, slice, selection, call, function, constant,
    context_reference>;

struct expression final {
    source_span span;
    expression_value value;
    size_t height = 1;
};

struct empty_statement { };

struct expression_statement {
    expression_ptr value;
};

struct block {
    std::vector<statement_ptr> statements;
    bool file = false;
};

struct branch {
    lexeme keyword;
    expression_ptr condition;
    statement_ptr yes, no;
};

struct while_loop {
    lexeme keyword;
    expression_ptr condition;
    statement_ptr body;
};

struct for_loop {
    lexeme keyword;
    expression_ptr setup, condition, step;
    statement_ptr body;
};

struct jump {
    lexeme keyword;
    expression_ptr value;
    statement_ptr conditional;
    std::optional<lexeme> target;
};

struct label {
    lexeme name;
};

struct definition {
    lexeme name;
    expression_ptr value;
};

struct catch_clause {
    lexeme keyword, binding;
    statement_ptr body;
};

struct try_statement {
    lexeme keyword;
    statement_ptr body;
    std::vector<catch_clause> handlers;
    statement_ptr finalizer;
};

using statement_value = std::variant<
    empty_statement, expression_statement, block, branch, while_loop, for_loop,
    jump, label, definition, try_statement>;

struct statement final {
    source_span span;
    statement_value value;
    size_t height = 1;
};

/// Internal deterministic resource limits; no user-facing compiler flags.
struct limits {
    size_t depth = 256;
    size_t nodes = 100000;
};

/// Normalize exactly one expression, rejecting unconsumed tokens.
expression_ptr normalize_expression(source_span input, limits budget = {});
/// Normalize a complete statement sequence without executing it.
statement_ptr normalize_program(source_span input, limits budget = {});
/// Reread exactly the selected source-backed group, including placeholders.
expression_ptr
normalize_expression(const group_node& group, limits budget = {});
statement_ptr normalize_program(const group_node& group, limits budget = {});
void dump_tokens(std::ostream& output, source_span input);
/// Visit immediate operand edges in evaluation order, skipping omitted bounds.
void for_each_child(
    const expression& value,
    const std::function<void(const expression_ptr&)>& expression_visitor,
    const std::function<void(const statement_ptr&)>& statement_visitor = {}
);
void for_each_child(
    const statement& value,
    const std::function<void(const expression_ptr&)>& expression_visitor,
    const std::function<void(const statement_ptr&)>& statement_visitor
);
void dump(std::ostream& output, const expression_ptr& value);
void dump(std::ostream& output, const statement_ptr& value);
}

#endif // NORMALIZED_HPP
