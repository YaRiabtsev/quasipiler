#include "evaluate.hpp"
#include "graph.hpp"

#include <array>
#include <bit>
#include <type_traits>
#include <unordered_set>

namespace runtime {
namespace {
    bool supported_binary(std::string_view op) {
        static constexpr std::array operators {
            "+", "-", "*", "/", "==", "!=", "<", "<=", ">", ">=", "&&", "||"
        };
        for (const auto* candidate : operators)
            if (candidate == op)
                return true;
        return false;
    }

    class evaluator {
    public:
        evaluator(const resolver& bindings, budget& work, reference_frame frame)
            : bindings_(bindings)
            , work_(work)
            , frame_(std::move(frame)) { }

        evaluation
        evaluate(const normalized::expression_ptr& tree, size_t depth = 0) {
            return { run(tree, depth), work_.steps_used(),
                     work_.elements_used(), work_.bytes_used() };
        }

        value resolve(value input, size_t depth = 0) {
            if (depth >= work_.options().depth)
                throw error(
                    error_code::resource,
                    "reference resolution depth limit exceeded"
                );
            if (input.kind() != value_kind::graph)
                return checked(std::move(input));
            const auto node = input.as_graph();
            if (node.kind() == graph_kind::list
                || node.kind() == graph_kind::object)
                return checked(std::move(input));
            work_.spend();
            if (node.kind() == graph_kind::scalar) {
                auto scalar = node.scalar();
                if (scalar.kind() == value_kind::string)
                    work_.bytes(scalar.as_string().size());
                return checked(std::move(scalar));
            }
            for (const auto& active : active_)
                if (node.same_identity(active.node)
                    && same_context(frame_.current, active.current))
                    throw error(
                        error_code::reference_cycle,
                        "reference recipe re-entered in the same context",
                        node.recipe()->span
                    );
            active_.push_back({ node, frame_.current });
            try {
                auto result = run(node.recipe(), depth + 1);
                active_.pop_back();
                return result;
            } catch (...) {
                active_.pop_back();
                throw;
            }
        }

    private:
        const resolver& bindings_;
        budget& work_;
        reference_frame frame_;

        struct active_reference {
            graph_handle node;
            std::optional<value> current;
        };

        std::vector<active_reference> active_;

        static bool same_context(
            const std::optional<value>& a, const std::optional<value>& b
        ) {
            if (!a || !b)
                return !a && !b;
            if (a->kind() != b->kind())
                return false;
            switch (a->kind()) {
            case value_kind::graph:
                return a->as_graph().same_identity(b->as_graph());
            // Private immutable storage identity is only a resolution-state
            // key; it does not become observable language collection identity.
            case value_kind::list:
                return &a->elements() == &b->elements();
            case value_kind::object:
                return &a->entries() == &b->entries();
            case value_kind::null:
                return true;
            case value_kind::boolean:
                return a->as_bool() == b->as_bool();
            case value_kind::integer:
            case value_kind::rational:
                return a->as_number() == b->as_number();
            case value_kind::real:
                return std::bit_cast<uint64_t>(a->as_real())
                    == std::bit_cast<uint64_t>(b->as_real());
            case value_kind::string:
                return a->as_string() == b->as_string();
            case value_kind::builtin:
                return a->as_builtin() == b->as_builtin();
            }
            return false;
        }

        template <class Action>
        value with_current(const value& parent, Action action) {
            auto previous = std::move(frame_.current);
            frame_.current = parent;
            try {
                auto result = action();
                frame_.current = std::move(previous);
                return result;
            } catch (...) {
                frame_.current = std::move(previous);
                throw;
            }
        }

        value selected(const value& base, const value& key, size_t depth) {
            work_.spend();
            return with_current(base, [&] {
                auto result = access(base, key);
                if (result.kind() == value_kind::string)
                    work_.bytes(result.as_string().size());
                return resolve(std::move(result), depth);
            });
        }

        value checked(value result) {
            if (result.depth() > work_.options().depth)
                throw error(
                    error_code::resource, "runtime value depth limit exceeded"
                );
            if ((result.kind() == value_kind::integer
                 || result.kind() == value_kind::rational)
                && result.as_number().bits() > work_.options().numbers.bits)
                throw error(
                    error_code::resource, "exact number limit exceeded"
                );
            return result;
        }

        value scalar(const normalized::scalar& data) {
            return std::visit(
                [&](const auto& item) -> value {
                    using T = std::decay_t<decltype(item)>;
                    if constexpr (std::is_same_v<T, std::monostate>)
                        return value();
                    else {
                        if constexpr (std::is_same_v<T, std::string>)
                            work_.bytes(item.size());
                        return value(item);
                    }
                },
                data
            );
        }

        value run(const normalized::expression_ptr& tree, size_t depth) {
            if (!tree)
                throw std::invalid_argument("cannot execute a null expression");
            try {
                if (depth >= work_.options().depth)
                    throw error(
                        error_code::resource, "expression depth limit exceeded"
                    );
                work_.spend();
                auto result = std::visit(
                    [&](const auto& node) -> value {
                        using T = std::decay_t<decltype(node)>;
                        if constexpr (std::is_same_v<T, normalized::literal>) {
                            switch (node.kind) {
                            case normalized::literal_kind::integer:
                            case normalized::literal_kind::decimal:
                                return numeric_literal(
                                    node.token.text, work_.options().numbers
                                );
                            case normalized::literal_kind::boolean:
                                return value(node.token.text == "true");
                            case normalized::literal_kind::null:
                                return value();
                            case normalized::literal_kind::string:
                                work_.bytes(node.token.text.size());
                                return value(node.token.text);
                            }
                        } else if constexpr (
                            std::is_same_v<T, normalized::constant>
                        ) {
                            return scalar(node.value);
                        } else if constexpr (
                            std::is_same_v<T, normalized::context_reference>
                        ) {
                            const auto& selected
                                = node.base == normalized::reference_base::root
                                ? frame_.root
                                : frame_.current;
                            if (!selected)
                                throw error(
                                    error_code::invalid_reference,
                                    "reference context is not bound"
                                );
                            if (selected->kind() == value_kind::string)
                                work_.bytes(selected->as_string().size());
                            return *selected;
                        } else if constexpr (
                            std::is_same_v<T, normalized::identifier>
                        ) {
                            auto found = bindings_(node.name.text);
                            if (!found)
                                throw error(
                                    error_code::unknown_name,
                                    "required name has no binding"
                                );
                            if (found->kind() == value_kind::string)
                                work_.bytes(found->as_string().size());
                            return std::move(*found);
                        } else if constexpr (
                            std::is_same_v<T, normalized::unary>
                        ) {
                            if (!node.prefix
                                || (node.op.text != "+" && node.op.text != "-"
                                    && node.op.text != "!"))
                                throw error(
                                    error_code::unsupported_operation,
                                    "unsupported unary expression"
                                );
                            const auto operand = run(node.operand, depth + 1);
                            return unary(
                                node.op.text, operand, work_.options().numbers
                            );
                        } else if constexpr (
                            std::is_same_v<T, normalized::binary>
                        ) {
                            if (!supported_binary(node.op.text))
                                throw error(
                                    error_code::unsupported_operation,
                                    "unsupported binary expression"
                                );
                            const auto left = run(node.left, depth + 1);
                            if (node.op.text == "&&" || node.op.text == "||") {
                                const bool condition = left.as_bool();
                                if ((node.op.text == "&&" && !condition)
                                    || (node.op.text == "||" && condition))
                                    return left;
                            }
                            const auto right = run(node.right, depth + 1);
                            return binary(
                                node.op.text, left, right,
                                work_.options().numbers
                            );
                        } else if constexpr (
                            std::is_same_v<T, normalized::ternary>
                        ) {
                            const auto condition
                                = run(node.condition, depth + 1);
                            return run(
                                condition.as_bool() ? node.yes : node.no,
                                depth + 1
                            );
                        } else if constexpr (
                            std::is_same_v<T, normalized::list>
                        ) {
                            work_.elements(node.elements.size());
                            std::vector<value> items;
                            items.reserve(node.elements.size());
                            for (const auto& child : node.elements)
                                items.push_back(run(child, depth + 1));
                            return value::list(std::move(items));
                        } else if constexpr (
                            std::is_same_v<T, normalized::object>
                        ) {
                            work_.elements(node.entries.size());
                            std::unordered_set<std::string_view> keys;
                            std::vector<std::pair<std::string, value>> items;
                            items.reserve(node.entries.size());
                            keys.reserve(node.entries.size());
                            for (const auto& entry : node.entries) {
                                work_.bytes(entry.key.text.size());
                                if (!keys.insert(entry.key.text).second)
                                    throw error(
                                        error_code::duplicate_key,
                                        "object contains a duplicate key",
                                        entry.key.span
                                    );
                                auto item = run(entry.value, depth + 1);
                                items.emplace_back(
                                    entry.key.text, std::move(item)
                                );
                            }
                            return value::object(std::move(items));
                        } else if constexpr (
                            std::is_same_v<T, normalized::member>
                        ) {
                            const auto base = run(node.base, depth + 1);
                            // Keep member type errors distinct from list index
                            // errors; dot access always requires an object.
                            return with_current(base, [&] {
                                auto item = base.member(node.name.text);
                                if (item.kind() == value_kind::string)
                                    work_.bytes(item.as_string().size());
                                return resolve(std::move(item), depth);
                            });
                        } else if constexpr (
                            std::is_same_v<T, normalized::index>
                        ) {
                            const auto base = run(node.base, depth + 1);
                            const auto key = with_current(base, [&] {
                                return run(node.subscript, depth + 1);
                            });
                            return selected(base, key, depth);
                        } else if constexpr (
                            std::is_same_v<T, normalized::selection>
                            || std::is_same_v<T, normalized::member_selection>
                        ) {
                            const auto base = run(node.base, depth + 1);
                            std::vector<value> items;
                            if constexpr (
                                std::is_same_v<T, normalized::selection>
                            ) {
                                work_.elements(node.keys.size());
                                items.reserve(node.keys.size());
                                for (const auto& key_tree : node.keys) {
                                    const auto key = with_current(base, [&] {
                                        return run(key_tree, depth + 1);
                                    });
                                    items.push_back(selected(base, key, depth));
                                }
                            } else {
                                work_.elements(node.names.size());
                                items.reserve(node.names.size());
                                for (const auto& name : node.names) {
                                    work_.spend();
                                    items.push_back(with_current(base, [&] {
                                        auto item = base.member(name.text);
                                        if (item.kind() == value_kind::string)
                                            work_.bytes(
                                                item.as_string().size()
                                            );
                                        return resolve(std::move(item), depth);
                                    }));
                                }
                            }
                            return value::list(std::move(items));
                        } else if constexpr (
                            std::is_same_v<T, normalized::call>
                        ) {
                            const auto callee = run(node.callee, depth + 1);
                            work_.elements(node.arguments.size());
                            std::vector<value> arguments;
                            arguments.reserve(node.arguments.size());
                            for (const auto& argument : node.arguments)
                                arguments.push_back(run(argument, depth + 1));
                            const auto identity = callee.as_builtin();
                            if ((identity == builtin_id::minimum
                                 || identity == builtin_id::maximum)
                                && arguments.size() == 1
                                && arguments.front().kind() == value_kind::graph
                                && arguments.front().as_graph().kind()
                                    == graph_kind::list) {
                                const auto base = arguments.front();
                                work_.elements(base.size());
                                std::vector<value> items;
                                items.reserve(base.size());
                                for (size_t i = 0; i < base.size(); ++i) {
                                    work_.spend();
                                    items.push_back(with_current(base, [&] {
                                        return resolve(
                                            value::graph(base.as_graph().at(i)),
                                            depth
                                        );
                                    }));
                                }
                                arguments.front()
                                    = value::list(std::move(items));
                            }
                            return invoke(identity, arguments, work_);
                        }
                        // Tuples, functions, slices, and mutation
                        // await their stages. Their operand/body syntax is not
                        // executed.
                        throw error(
                            error_code::unsupported_operation,
                            "expression is outside the base runtime"
                        );
                    },
                    tree->value
                );
                return resolve(std::move(result), depth);
            } catch (const error& failure) {
                throw failure.located(tree->span);
            }
        }
    };
}

environment standard_environment() {
    environment bindings;
    for (const auto& builtin : builtins())
        bindings.emplace(builtin.name, value::builtin(builtin.identity));
    return bindings;
}

evaluation evaluate(
    const normalized::expression_ptr& expression, const environment& bindings,
    limits options
) {
    return evaluate_with(
        expression,
        [&](std::string_view name) -> std::optional<value> {
            const auto found = bindings.find(name);
            return found == bindings.end() ? std::nullopt
                                           : std::optional(found->second);
        },
        options
    );
}

evaluation evaluate_with(
    const normalized::expression_ptr& expression, const resolver& bindings,
    limits options
) {
    if (!bindings)
        throw std::invalid_argument("runtime resolver is empty");
    return evaluate_with_context(expression, {}, bindings, options);
}

evaluation evaluate_in(
    const normalized::expression_ptr& expression, reference_frame frame,
    const environment& bindings, limits options
) {
    return evaluate_with_context(
        expression, std::move(frame),
        [&](std::string_view name) -> std::optional<value> {
            const auto found = bindings.find(name);
            return found == bindings.end() ? std::nullopt
                                           : std::optional(found->second);
        },
        options
    );
}

evaluation evaluate_with_context(
    const normalized::expression_ptr& expression, reference_frame frame,
    const resolver& bindings, limits options
) {
    if (!bindings)
        throw std::invalid_argument("runtime resolver is empty");
    budget work(options);
    return evaluator(bindings, work, std::move(frame)).evaluate(expression);
}

value resolve_references(
    value input, reference_frame frame, const resolver& bindings, budget& work,
    size_t enclosing_depth
) {
    if (!bindings)
        throw std::invalid_argument("runtime resolver is empty");
    return evaluator(bindings, work, std::move(frame))
        .resolve(std::move(input), enclosing_depth);
}

value evaluate_bounded(
    const normalized::expression_ptr& expression, reference_frame frame,
    const resolver& bindings, budget& work, size_t enclosing_depth
) {
    if (!bindings)
        throw std::invalid_argument("runtime resolver is empty");
    return evaluator(bindings, work, std::move(frame))
        .evaluate(expression, enclosing_depth)
        .output;
}
}
