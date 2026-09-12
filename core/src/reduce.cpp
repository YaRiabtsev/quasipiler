#include "reduce.hpp"
#include "normalized_format.hpp"
#include "operators.hpp"

#include <algorithm>
#include <ostream>
#include <stdexcept>
#include <type_traits>

namespace reduction {
using namespace normalized;

namespace {
    struct exhausted { };

    template <typename T> const T* known(const result& value) {
        const auto* scalar = value.value();
        return scalar ? std::get_if<T>(scalar) : nullptr;
    }

    bool has_type(const result& value, input_type type) {
        if (value.input)
            return value.input->type == type;
        switch (type) {
        case input_type::exact_number:
            return known<exact::number>(value) != nullptr;
        case input_type::boolean:
            return known<bool>(value) != nullptr;
        case input_type::string:
            return known<std::string>(value) != nullptr;
        default:
            return false;
        }
    }

    reason numeric_reason(exact::failure failure) {
        switch (failure) {
        case exact::failure::growth:
            return reason::numeric_growth;
        case exact::failure::division_by_zero:
            return reason::division_by_zero;
        default:
            return reason::invalid_literal;
        }
    }

    class reducer {
    public:
        reducer(
            const environment& bindings, limits budget,
            reporting reports = reporting::none
        )
            : bindings_(bindings)
            , budget_(budget)
            , reports_(reports) {
            exact::validate(budget.numbers);
            if (budget.depth > 512 || budget.steps > 1000000)
                throw std::invalid_argument(
                    "reducer limits outside supported range"
                );
        }

        result run(const expression_ptr& tree, size_t depth = 0) {
            if (!tree)
                throw std::invalid_argument("cannot reduce a null expression");
            take_step(depth);
            if (const auto* value = std::get_if<constant>(&tree->value)) {
                if (const auto* number
                    = std::get_if<exact::number>(&value->value);
                    number && number->bits() > budget_.numbers.bits)
                    return residual(tree, flag(reason::numeric_growth));
                return { tree, state::known, 0, {} };
            }
            if (const auto* value = std::get_if<literal>(&tree->value)) {
                switch (value->kind) {
                case literal_kind::integer:
                case literal_kind::decimal: {
                    auto parsed
                        = exact::parse(value->token.text, budget_.numbers);
                    return parsed.value
                        ? folded(tree, *parsed.value)
                        : residual(tree, flag(numeric_reason(parsed.error)));
                }
                case literal_kind::boolean:
                    return folded(tree, value->token.text == "true");
                case literal_kind::string:
                    return folded(tree, value->token.text);
                case literal_kind::null:
                    return folded(tree, std::monostate {});
                }
            }
            if (const auto* name = std::get_if<identifier>(&tree->value)) {
                const auto found = bindings_.find(name->name.text);
                if (found == bindings_.end())
                    return { tree,
                             state::unresolved,
                             flag(reason::unknown_symbol),
                             {} };
                if (const auto* value = std::get_if<scalar>(&found->second)) {
                    if (const auto* number = std::get_if<exact::number>(value);
                        number && number->bits() > budget_.numbers.bits)
                        return residual(tree, flag(reason::numeric_growth));
                    return folded(tree, *value);
                }
                return { tree, state::input, flag(reason::input_dependency),
                         std::get<input_binding>(found->second) };
            }
            if (const auto* node = std::get_if<unary>(&tree->value)) {
                // A write target is syntax, never a value substitution site.
                if (node->op.text == "++" || node->op.text == "--")
                    return residual(tree, flag(reason::side_effect));
                const auto operand = run(node->operand, depth + 1);
                auto rewritten = *node;
                rewritten.operand = operand.tree;
                const auto updated
                    = rebuild(tree, rewritten, operand.tree != node->operand);
                if (node->prefix && node->op.text == "+"
                    && has_type(operand, input_type::exact_number))
                    return operand;
                if (node->prefix && node->op.text == "-") {
                    if (const auto* number = known<exact::number>(operand)) {
                        auto negated = exact::negate(*number, budget_.numbers);
                        return negated.value
                            ? folded(tree, *negated.value)
                            : residual(
                                  updated,
                                  operand.why
                                      | flag(numeric_reason(negated.error))
                              );
                    }
                } else if (node->prefix && node->op.text == "!") {
                    if (const auto* boolean = known<bool>(operand))
                        return folded(tree, !*boolean);
                } else if (node->op.text != "+")
                    return residual(
                        updated,
                        operand.why | flag(reason::unsupported_operation)
                    );
                return residual(
                    updated,
                    operand.why
                        | (operand.value() ? flag(reason::incompatible_types)
                                           : 0)
                );
            }
            if (const auto* node = std::get_if<binary>(&tree->value))
                return binary_expression(tree, *node, depth);
            if (const auto* node = std::get_if<ternary>(&tree->value)) {
                const auto condition = run(node->condition, depth + 1);
                if (const auto* boolean = known<bool>(condition))
                    return run(*boolean ? node->yes : node->no, depth + 1);
                const auto yes = run(node->yes, depth + 1);
                const auto no = run(node->no, depth + 1);
                auto rewritten = *node;
                rewritten.condition = condition.tree;
                rewritten.yes = yes.tree;
                rewritten.no = no.tree;
                return residual(
                    rebuild(
                        tree, rewritten,
                        condition.tree != node->condition
                            || yes.tree != node->yes || no.tree != node->no
                    ),
                    condition.why | yes.why | no.why
                        | (condition.value() ? flag(reason::incompatible_types)
                                             : 0)
                );
            }
            // Function creation never evaluates its body or resolves its
            // locals.
            if (std::holds_alternative<function>(tree->value))
                return residual(tree, flag(reason::unsupported_operation));
            // Bound a collection's temporary child-vector copy as well as
            // recursive visits. Stop counting as soon as the budget fails.
            size_t children = 0;
            for_each_child(*tree, [&](const expression_ptr&) {
                if (++children > budget_.steps - steps_)
                    throw exhausted {};
            });
            reasons why = flag(reason::unsupported_operation);
            if (std::holds_alternative<call>(tree->value))
                why |= flag(reason::side_effect);
            bool changed = false;
            auto child = [&](expression_ptr& value) {
                if (!value)
                    return;
                auto reduced = run(value, depth + 1);
                changed |= value != reduced.tree;
                why |= reduced.why;
                value = std::move(reduced.tree);
            };
            auto rewritten = tree->value;
            std::visit(
                [&](auto& node) {
                    using T = std::decay_t<decltype(node)>;
                    if constexpr (
                        std::is_same_v<T, list> || std::is_same_v<T, tuple>
                    ) {
                        for (auto& item : node.elements)
                            child(item);
                    } else if constexpr (std::is_same_v<T, object>) {
                        for (auto& item : node.entries)
                            child(item.value);
                    } else if constexpr (
                        std::is_same_v<T, member>
                        || std::is_same_v<T, member_selection>
                    ) {
                        child(node.base);
                    } else if constexpr (std::is_same_v<T, index>) {
                        child(node.base);
                        child(node.subscript);
                    } else if constexpr (std::is_same_v<T, slice>) {
                        child(node.base);
                        child(node.start);
                        child(node.stop);
                        child(node.step);
                    } else if constexpr (std::is_same_v<T, selection>) {
                        child(node.base);
                        for (auto& key : node.keys)
                            child(key);
                    } else if constexpr (std::is_same_v<T, call>) {
                        child(node.callee);
                        for (auto& arg : node.arguments)
                            child(arg);
                    }
                },
                rewritten
            );
            return residual(rebuild(tree, std::move(rewritten), changed), why);
        }

        program_result program(const statement_ptr& tree) {
            const auto reduced = statement_tree(tree, 0);
            return { reduced, program_reasons_, std::move(sites_) };
        }

    private:
        const environment& bindings_;
        limits budget_;
        reporting reports_;
        size_t steps_ = 0;
        reasons program_reasons_ = 0;
        std::vector<site> sites_;

        void take_step(size_t depth) {
            if (depth >= budget_.depth || steps_ >= budget_.steps)
                throw exhausted {};
            ++steps_;
        }

        statement_ptr statement_tree(const statement_ptr& tree, size_t depth) {
            if (!tree)
                throw std::invalid_argument("cannot reduce a null program");
            take_step(depth);
            size_t children = 0;
            auto count = [&](const auto&) {
                if (++children > budget_.steps - steps_)
                    throw exhausted {};
            };
            for_each_child(*tree, count, count);
            bool changed = false;
            auto expr = [&](expression_ptr& value) {
                if (!value)
                    return;
                auto reduced = run(value, depth + 1);
                if (reports_ == reporting::expressions)
                    sites_.push_back({ value->span, reduced });
                program_reasons_ |= reduced.why;
                changed |= reduced.tree != value;
                value = std::move(reduced.tree);
            };
            auto stmt = [&](statement_ptr& value) {
                if (!value)
                    return;
                const auto reduced = statement_tree(value, depth + 1);
                changed |= reduced != value;
                value = reduced;
            };
            auto rewritten = tree->value;
            std::visit(
                [&](auto& node) {
                    using T = std::decay_t<decltype(node)>;
                    if constexpr (
                        std::is_same_v<T, expression_statement>
                        || std::is_same_v<T, definition>
                    )
                        expr(node.value);
                    else if constexpr (std::is_same_v<T, block>) {
                        for (auto& item : node.statements)
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
                        for (auto& handler : node.handlers)
                            stmt(handler.body);
                        stmt(node.finalizer);
                    }
                },
                rewritten
            );
            if (!changed)
                return tree;
            statement reduced { tree->span, std::move(rewritten), 1 };
            auto height = [&](const auto& child) {
                reduced.height = std::max(reduced.height, child->height + 1);
            };
            for_each_child(reduced, height, height);
            return std::make_shared<const statement>(std::move(reduced));
        }

        static result residual(expression_ptr tree, reasons why) {
            return { std::move(tree), state::residual, why, {} };
        }

        static result folded(const expression_ptr& tree, scalar value) {
            return { std::make_shared<const expression>(expression {
                         tree->span, constant { std::move(value) }, 1 }),
                     state::known,
                     0,
                     {} };
        }

        static expression_ptr rebuild(
            const expression_ptr& original, expression_value value, bool changed
        ) {
            if (!changed)
                return original;
            expression rebuilt { original->span, std::move(value), 1 };
            for_each_child(
                rebuilt,
                [&](const expression_ptr& child) {
                    rebuilt.height
                        = std::max(rebuilt.height, child->height + 1);
                },
                [&](const statement_ptr& child) {
                    rebuilt.height
                        = std::max(rebuilt.height, child->height + 1);
                }
            );
            return std::make_shared<const expression>(std::move(rebuilt));
        }

        result binary_expression(
            const expression_ptr& tree, const binary& node, size_t depth
        ) {
            const auto& op = node.op.text;
            if (const auto* info = binary_operator(op);
                info && info->precedence == 1) {
                // Preserve the complete lvalue (including its effects). Only
                // the RHS is an ordinary value context; no assignment is
                // executed.
                const auto right = run(node.right, depth + 1);
                auto rewritten = node;
                rewritten.right = right.tree;
                return residual(
                    rebuild(tree, rewritten, right.tree != node.right),
                    right.why | flag(reason::side_effect)
                );
            }
            const auto left = run(node.left, depth + 1);
            const auto* boolean = known<bool>(left);
            if ((op == "&&" && boolean && !*boolean)
                || (op == "||" && boolean && *boolean))
                return folded(tree, *boolean);
            const auto right = run(node.right, depth + 1);
            auto rewritten = node;
            rewritten.left = left.tree;
            rewritten.right = right.tree;
            const auto updated = rebuild(
                tree, rewritten,
                left.tree != node.left || right.tree != node.right
            );
            const reasons why = left.why | right.why;
            if (op == "&&" || op == "||") {
                if (boolean && has_type(right, input_type::boolean))
                    return right;
                const bool incompatible = (left.value() && !boolean)
                    || (right.value() && !known<bool>(right));
                return residual(
                    updated,
                    why | (incompatible ? flag(reason::incompatible_types) : 0)
                );
            }
            const auto* a = known<exact::number>(left);
            const auto* b = known<exact::number>(right);
            if (op == "+" || op == "-" || op == "*" || op == "/") {
                if (a && b) {
                    const auto operation = op == "+" ? exact::operation::add
                        : op == "-" ? exact::operation::subtract
                        : op == "*" ? exact::operation::multiply
                                    : exact::operation::divide;
                    auto value
                        = exact::calculate(operation, *a, *b, budget_.numbers);
                    return value.value
                        ? folded(tree, *value.value)
                        : residual(
                              updated, why | flag(numeric_reason(value.error))
                          );
                }
                if (has_type(left, input_type::exact_number) && b
                    && (((op == "+" || op == "-") && b->is_zero())
                        || ((op == "*" || op == "/") && b->is_one())))
                    return left;
                if (a && has_type(right, input_type::exact_number)
                    && ((op == "+" && a->is_zero())
                        || (op == "*" && a->is_one())))
                    return right;
                const bool incompatible
                    = (left.value() && !a) || (right.value() && !b);
                return residual(
                    updated,
                    why | (incompatible ? flag(reason::incompatible_types) : 0)
                );
            }
            if (op == "==" || op == "!=") {
                if (left.value() && right.value()) {
                    if (left.value()->index() != right.value()->index())
                        return residual(
                            updated, flag(reason::incompatible_types)
                        );
                    const bool equal = *left.value() == *right.value();
                    return folded(tree, op == "==" ? equal : !equal);
                }
                return residual(updated, why);
            }
            if (op == "<" || op == "<=" || op == ">" || op == ">=") {
                if (a && b) {
                    const int order = a->compare(*b);
                    return folded(
                        tree,
                        op == "<"        ? order < 0
                            : op == "<=" ? order <= 0
                            : op == ">"  ? order > 0
                                         : order >= 0
                    );
                }
                return residual(
                    updated,
                    why
                        | ((left.value() && !a) || (right.value() && !b)
                               ? flag(reason::incompatible_types)
                               : 0)
                );
            }
            return residual(updated, why | flag(reason::unsupported_operation));
        }
    };
}

const scalar* result::value() const {
    if (kind != state::known || !tree)
        return nullptr;
    const auto* value = std::get_if<constant>(&tree->value);
    return value ? &value->value : nullptr;
}

result reduce(
    const expression_ptr& expression, const environment& bindings, limits budget
) {
    try {
        return reducer(bindings, budget).run(expression);
    } catch (const exhausted&) {
        return {
            expression, state::residual, flag(reason::expression_growth), {}
        };
    }
}

namespace {
    void dump_reasons(std::ostream& output, reasons why) {
        static constexpr const char* causes[]
            = { "unknown_symbol", "input_dependency",   "unsupported_operation",
                "side_effect",    "incompatible_types", "division_by_zero",
                "numeric_growth", "expression_growth",  "invalid_literal" };
        output << '[';
        bool separator = false;
        for (size_t i = 0; i < std::size(causes); ++i) {
            if ((why & (1U << i)) != 0) {
                if (separator)
                    output << ',';
                output << causes[i];
                separator = true;
            }
        }
        output << ']';
    }
}

void dump(std::ostream& output, const result& value) {
    static constexpr const char* states[]
        = { "known", "input", "unresolved", "residual" };
    output << states[static_cast<size_t>(value.kind)];
    if (value.input)
        output << ' ' << normalized::detail::quoted_text(value.input->identity);
    output << ' ';
    dump_reasons(output, value.why);
    output << ' ';
    normalized::dump(output, value.tree);
}

program_result
reduce_program(const statement_ptr& program, limits budget, reporting reports) {
    const environment bindings;
    try {
        return reducer(bindings, budget, reports).program(program);
    } catch (const exhausted&) {
        return { program, flag(reason::expression_growth), {} };
    }
}

void dump(std::ostream& output, const program_result& value) {
    output << "program ";
    dump_reasons(output, value.why);
    output << '\n';
    for (const auto& site : value.expressions) {
        output << site.original.begin().offset << ':'
               << site.original.end().offset << ' ';
        dump(output, site.value);
    }
}
}
