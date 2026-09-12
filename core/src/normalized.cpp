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

#include "normalized.hpp"
#include "normalized_format.hpp"

#include <ostream>
#include <type_traits>

namespace normalized {
namespace {
    void print_statement(std::ostream& out, const statement_ptr& value);

    void print(std::ostream& out, const expression_ptr& expr) {
        if (!expr) {
            out << "_";
            return;
        }
        std::visit(
            [&](const auto& node) {
                using T = std::decay_t<decltype(node)>;
                auto children = [&](const auto& values) {
                    for (const auto& child : values) {
                        out << ' ';
                        print(out, child);
                    }
                };
                if constexpr (std::is_same_v<T, literal>) {
                    static constexpr const char* names[]
                        = { "integer", "decimal", "string", "boolean", "null" };
                    out << '(' << names[static_cast<size_t>(node.kind)] << ' '
                        << detail::quoted_text(node.token.text) << ')';
                } else if constexpr (std::is_same_v<T, constant>) {
                    std::visit(
                        [&](const auto& value) {
                            using V = std::decay_t<decltype(value)>;
                            if constexpr (std::is_same_v<V, exact::number>)
                                out << "(exact "
                                    << detail::quoted_text(value.str()) << ')';
                            else if constexpr (std::is_same_v<V, bool>)
                                out << "(boolean \""
                                    << (value ? "true" : "false") << "\")";
                            else if constexpr (std::is_same_v<V, std::string>)
                                out << "(string " << detail::quoted_text(value)
                                    << ')';
                            else
                                out << "(null \"null\")";
                        },
                        node.value
                    );
                } else if constexpr (std::is_same_v<T, identifier>) {
                    out << "(id " << detail::quoted_text(node.name.text) << ')';
                } else if constexpr (std::is_same_v<T, context_reference>) {
                    out
                        << (node.base == reference_base::root ? "(root)"
                                                              : "(current)");
                } else if constexpr (std::is_same_v<T, unary>) {
                    out << '(' << (node.prefix ? "prefix " : "postfix ")
                        << node.op.text << ' ';
                    print(out, node.operand);
                    out << ')';
                } else if constexpr (std::is_same_v<T, binary>) {
                    out << '(' << node.op.text << ' ';
                    print(out, node.left);
                    out << ' ';
                    print(out, node.right);
                    out << ')';
                } else if constexpr (std::is_same_v<T, ternary>) {
                    out << "(?: ";
                    print(out, node.condition);
                    out << ' ';
                    print(out, node.yes);
                    out << ' ';
                    print(out, node.no);
                    out << ')';
                } else if constexpr (
                    std::is_same_v<T, list> || std::is_same_v<T, tuple>
                ) {
                    out << (std::is_same_v<T, list> ? "(list" : "(tuple");
                    children(node.elements);
                    out << ')';
                } else if constexpr (std::is_same_v<T, object>) {
                    out << "(object";
                    for (const auto& entry : node.entries) {
                        out << " (" << detail::quoted_text(entry.key.text)
                            << ' ';
                        print(out, entry.value);
                        out << ')';
                    }
                    out << ')';
                } else if constexpr (std::is_same_v<T, member>) {
                    out << "(member ";
                    print(out, node.base);
                    out << ' ' << detail::quoted_text(node.name.text) << ')';
                } else if constexpr (std::is_same_v<T, member_selection>) {
                    out << "(members ";
                    print(out, node.base);
                    for (const auto& name : node.names)
                        out << ' ' << detail::quoted_text(name.text);
                    out << ')';
                } else if constexpr (std::is_same_v<T, index>) {
                    out << "(index ";
                    print(out, node.base);
                    out << ' ';
                    print(out, node.subscript);
                    out << ')';
                } else if constexpr (std::is_same_v<T, slice>) {
                    out << "(slice ";
                    print(out, node.base);
                    out << ' ';
                    print(out, node.start);
                    out << ' ';
                    print(out, node.stop);
                    out << ' ';
                    print(out, node.step);
                    out << ')';
                } else if constexpr (std::is_same_v<T, selection>) {
                    out << "(select ";
                    print(out, node.base);
                    children(node.keys);
                    out << ')';
                } else if constexpr (std::is_same_v<T, call>) {
                    out << "(call ";
                    print(out, node.callee);
                    children(node.arguments);
                    out << ')';
                } else if constexpr (std::is_same_v<T, function>) {
                    out << "(function (parameters";
                    for (const auto& parameter : node.parameters)
                        out << ' ' << detail::quoted_text(parameter.text);
                    out << ") ";
                    print_statement(out, node.body);
                    out << ')';
                }
            },
            expr->value
        );
    }

    void print_statement(std::ostream& out, const statement_ptr& value) {
        if (!value) {
            out << "_";
            return;
        }
        std::visit(
            [&](const auto& node) {
                using T = std::decay_t<decltype(node)>;
                if constexpr (std::is_same_v<T, empty_statement>)
                    out << "(empty)";
                else if constexpr (std::is_same_v<T, expression_statement>) {
                    out << "(expression ";
                    print(out, node.value);
                    out << ')';
                } else if constexpr (std::is_same_v<T, block>) {
                    out << (node.file ? "(program" : "(block");
                    for (const auto& child : node.statements) {
                        out << ' ';
                        print_statement(out, child);
                    }
                    out << ')';
                } else if constexpr (std::is_same_v<T, branch>) {
                    out << "(if ";
                    print(out, node.condition);
                    out << ' ';
                    print_statement(out, node.yes);
                    out << ' ';
                    print_statement(out, node.no);
                    out << ')';
                } else if constexpr (std::is_same_v<T, while_loop>) {
                    out << "(while ";
                    print(out, node.condition);
                    out << ' ';
                    print_statement(out, node.body);
                    out << ')';
                } else if constexpr (std::is_same_v<T, for_loop>) {
                    out << "(for ";
                    print(out, node.setup);
                    out << ' ';
                    print(out, node.condition);
                    out << ' ';
                    print(out, node.step);
                    out << ' ';
                    print_statement(out, node.body);
                    out << ')';
                } else if constexpr (std::is_same_v<T, jump>) {
                    out << '(' << node.keyword.text;
                    if (node.value) {
                        out << ' ';
                        print(out, node.value);
                    }
                    if (node.conditional) {
                        out << ' ';
                        print_statement(out, node.conditional);
                    }
                    if (node.target)
                        out << ' ' << detail::quoted_text(node.target->text);
                    out << ')';
                } else if constexpr (std::is_same_v<T, label>)
                    out << "(label " << detail::quoted_text(node.name.text)
                        << ')';
                else if constexpr (std::is_same_v<T, definition>) {
                    out << "(define " << detail::quoted_text(node.name.text)
                        << ' ';
                    print(out, node.value);
                    out << ')';
                } else if constexpr (std::is_same_v<T, try_statement>) {
                    out << "(try ";
                    print_statement(out, node.body);
                    for (const auto& handler : node.handlers) {
                        out << " (catch "
                            << detail::quoted_text(handler.binding.text) << ' ';
                        print_statement(out, handler.body);
                        out << ')';
                    }
                    if (node.finalizer) {
                        out << " (finally ";
                        print_statement(out, node.finalizer);
                        out << ')';
                    }
                    out << ')';
                }
            },
            value->value
        );
    }

}

void for_each_child(
    const expression& value,
    const std::function<void(const expression_ptr&)>& visitor,
    const std::function<void(const statement_ptr&)>& statement_visitor
) {
    auto child = [&](const expression_ptr& node) {
        if (node)
            visitor(node);
    };
    std::visit(
        [&](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, unary>)
                child(node.operand);
            else if constexpr (std::is_same_v<T, binary>) {
                child(node.left);
                child(node.right);
            } else if constexpr (std::is_same_v<T, ternary>) {
                child(node.condition);
                child(node.yes);
                child(node.no);
            } else if constexpr (
                std::is_same_v<T, list> || std::is_same_v<T, tuple>
            ) {
                for (const auto& item : node.elements)
                    child(item);
            } else if constexpr (std::is_same_v<T, object>) {
                for (const auto& item : node.entries)
                    child(item.value);
            } else if constexpr (
                std::is_same_v<T, member> || std::is_same_v<T, member_selection>
            )
                child(node.base);
            else if constexpr (std::is_same_v<T, index>) {
                child(node.base);
                child(node.subscript);
            } else if constexpr (std::is_same_v<T, slice>) {
                child(node.base);
                child(node.start);
                child(node.stop);
                child(node.step);
            } else if constexpr (std::is_same_v<T, selection>) {
                child(node.base);
                for (const auto& item : node.keys)
                    child(item);
            } else if constexpr (std::is_same_v<T, call>) {
                child(node.callee);
                for (const auto& item : node.arguments)
                    child(item);
            } else if constexpr (std::is_same_v<T, function>) {
                if (statement_visitor && node.body)
                    statement_visitor(node.body);
            }
        },
        value.value
    );
}

void for_each_child(
    const statement& value,
    const std::function<void(const expression_ptr&)>& expression_visitor,
    const std::function<void(const statement_ptr&)>& statement_visitor
) {
    auto expr = [&](const expression_ptr& p) {
        if (p)
            expression_visitor(p);
    };
    auto stmt = [&](const statement_ptr& p) {
        if (p)
            statement_visitor(p);
    };
    std::visit(
        [&](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (
                std::is_same_v<T, expression_statement>
                || std::is_same_v<T, definition>
            )
                expr(node.value);
            else if constexpr (std::is_same_v<T, block>) {
                for (const auto& item : node.statements)
                    stmt(item);
            } else if constexpr (std::is_same_v<T, branch>) {
                expr(node.condition);
                stmt(node.yes);
                stmt(node.no);
            } else if constexpr (std::is_same_v<T, while_loop>) {
                expr(node.condition);
                stmt(node.body);
            } else if constexpr (std::is_same_v<T, for_loop>) {
                expr(node.setup);
                expr(node.condition);
                expr(node.step);
                stmt(node.body);
            } else if constexpr (std::is_same_v<T, jump>) {
                expr(node.value);
                stmt(node.conditional);
            } else if constexpr (std::is_same_v<T, try_statement>) {
                stmt(node.body);
                for (const auto& handler : node.handlers)
                    stmt(handler.body);
                stmt(node.finalizer);
            }
        },
        value.value
    );
}

void dump(std::ostream& output, const statement_ptr& value) {
    print_statement(output, value);
    output << '\n';
}

void dump(std::ostream& output, const expression_ptr& value) {
    print(output, value);
    output << '\n';
}
}
