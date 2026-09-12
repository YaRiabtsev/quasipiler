#include "graph_io.hpp"

#include <array>
#include <charconv>

namespace runtime {
namespace {
    resolver names(const environment& bindings) {
        return [&bindings](std::string_view name) -> std::optional<value> {
            const auto found = bindings.find(name);
            return found == bindings.end() ? std::nullopt
                                           : std::optional(found->second);
        };
    }

    bool reference_path(normalized::expression_ptr node, budget& work) {
        while (node) {
            work.spend();
            if (std::holds_alternative<normalized::context_reference>(
                    node->value
                ))
                return true;
            node = std::visit(
                [](const auto& item) -> normalized::expression_ptr {
                    using T = std::decay_t<decltype(item)>;
                    if constexpr (
                        std::is_same_v<T, normalized::member>
                        || std::is_same_v<T, normalized::index>
                        || std::is_same_v<T, normalized::member_selection>
                        || std::is_same_v<T, normalized::selection>
                    )
                        return item.base;
                    else
                        return {};
                },
                node->value
            );
        }
        return false;
    }

    bool utf8(std::string_view text) {
        for (size_t i = 0; i < text.size();) {
            const auto lead = static_cast<unsigned char>(text[i++]);
            if (lead < 0x80)
                continue;
            size_t rest;
            uint32_t point;
            uint32_t minimum;
            if (lead >= 0xc2 && lead <= 0xdf) {
                rest = 1;
                point = lead & 0x1fU;
                minimum = 0x80;
            } else if (lead >= 0xe0 && lead <= 0xef) {
                rest = 2;
                point = lead & 0x0fU;
                minimum = 0x800;
            } else if (lead >= 0xf0 && lead <= 0xf4) {
                rest = 3;
                point = lead & 0x07U;
                minimum = 0x10000;
            } else
                return false;
            if (rest > text.size() - i)
                return false;
            for (size_t j = 0; j < rest; ++j) {
                const auto next = static_cast<unsigned char>(text[i++]);
                if ((next & 0xc0U) != 0x80U)
                    return false;
                point = (point << 6U) | (next & 0x3fU);
            }
            if (point < minimum || point > 0x10ffff
                || (point >= 0xd800 && point <= 0xdfff))
                return false;
        }
        return true;
    }

    bool collection(const value& item) {
        return item.kind() == value_kind::list
            || item.kind() == value_kind::object
            || (item.kind() == value_kind::graph
                && (item.as_graph().kind() == graph_kind::list
                    || item.as_graph().kind() == graph_kind::object));
    }

    bool object(const value& item) {
        return item.kind() == value_kind::object
            || (item.kind() == value_kind::graph
                && item.as_graph().kind() == graph_kind::object);
    }

    bool identical_collection(const value& a, const value& b) {
        if (a.kind() != b.kind())
            return false;
        if (a.kind() == value_kind::graph)
            return a.as_graph().same_identity(b.as_graph());
        if (a.kind() == value_kind::list)
            return &a.elements() == &b.elements();
        return &a.entries() == &b.entries();
    }

    class serializer {
    public:
        serializer(const environment& bindings, serialization_limits options)
            : bindings_(names(bindings))
            , work_(options.execution)
            , maximum_(options.output_bytes) {
            if (maximum_ > 16777216)
                throw std::invalid_argument(
                    "serialization output limit outside supported range"
                );
        }

        std::string run(const value& input, reference_frame context) {
            if (!context.root)
                context.root = input;
            if (!context.current)
                context.current = input;

            struct frame {
                value item;
                reference_frame context;
                bool entered = false;
                size_t cursor = 0;
            };

            std::vector<frame> pending;
            if (work_.options().depth == 0)
                throw error(
                    error_code::resource, "serialization depth limit exceeded"
                );
            pending.push_back({ input, std::move(context) });
            std::vector<value> active;
            while (!pending.empty()) {
                auto& current = pending.back();
                if (!current.entered) {
                    work_.spend();
                    work_.elements(1);
                    current.item = resolve_references(
                        std::move(current.item), current.context, bindings_,
                        work_, pending.size() - 1
                    );
                    if (!collection(current.item)) {
                        scalar(current.item);
                        pending.pop_back();
                        continue;
                    }
                    for (const auto& ancestor : active)
                        if (identical_collection(current.item, ancestor))
                            throw error(
                                error_code::serialization_cycle,
                                "JSON output contains an active-path "
                                "collection cycle"
                            );
                    active.push_back(current.item);
                    append(object(current.item) ? "{" : "[");
                    current.entered = true;
                }
                if (current.cursor == current.item.size()) {
                    append(object(current.item) ? "}" : "]");
                    active.pop_back();
                    pending.pop_back();
                    continue;
                }
                if (pending.size() >= work_.options().depth)
                    throw error(
                        error_code::resource,
                        "serialization depth limit exceeded"
                    );
                const auto index = current.cursor++;
                if (index)
                    append(",");
                value child;
                if (current.item.kind() == value_kind::graph) {
                    const auto& node = current.item.as_graph();
                    if (object(current.item)) {
                        quoted(node.key_at(index));
                        append(":");
                    }
                    child = value::graph(node.child_at(index));
                } else if (object(current.item)) {
                    const auto& entry = current.item.entries()[index];
                    quoted(entry.first);
                    append(":");
                    child = entry.second;
                } else
                    child = current.item.elements()[index];
                reference_frame next { current.context.root, current.item };
                pending.push_back({ std::move(child), std::move(next) });
            }
            return std::move(output_);
        }

    private:
        resolver bindings_;
        budget work_;
        size_t maximum_;
        std::string output_;

        void append(std::string_view text) {
            if (text.size() > maximum_ - output_.size())
                throw error(
                    error_code::resource,
                    "serialization output byte limit exceeded"
                );
            output_.append(text);
        }

        void quoted(std::string_view text) {
            if (text.size() > maximum_ - output_.size())
                throw error(
                    error_code::resource,
                    "serialization output byte limit exceeded"
                );
            if (!utf8(text))
                throw error(
                    error_code::conversion, "JSON strings require valid UTF-8"
                );
            append("\"");
            static constexpr char hex[] = "0123456789abcdef";
            for (const auto character : text) {
                switch (character) {
                case '"':
                    append("\\\"");
                    break;
                case '\\':
                    append("\\\\");
                    break;
                case '\n':
                    append("\\n");
                    break;
                case '\r':
                    append("\\r");
                    break;
                case '\t':
                    append("\\t");
                    break;
                case '\b':
                    append("\\b");
                    break;
                case '\f':
                    append("\\f");
                    break;
                default: {
                    const auto byte = static_cast<unsigned char>(character);
                    if (byte < 0x20) {
                        const std::array<char, 6> escaped { '\\',
                                                            'u',
                                                            '0',
                                                            '0',
                                                            hex[byte >> 4U],
                                                            hex[byte & 0xfU] };
                        append(
                            std::string_view(escaped.data(), escaped.size())
                        );
                    } else
                        append(std::string_view(&character, 1));
                }
                }
            }
            append("\"");
        }

        void scalar(const value& item) {
            switch (item.kind()) {
            case value_kind::null:
                append("null");
                return;
            case value_kind::boolean:
                append(item.as_bool() ? "true" : "false");
                return;
            case value_kind::integer:
                append(item.as_number().str());
                return;
            case value_kind::real: {
                std::array<char, 64> buffer;
                const auto [end, code] = std::to_chars(
                    buffer.data(), buffer.data() + buffer.size(), item.as_real()
                );
                if (code != std::errc())
                    throw error(
                        error_code::conversion,
                        "binary64 JSON formatting failed"
                    );
                append(
                    std::string_view(
                        buffer.data(), static_cast<size_t>(end - buffer.data())
                    )
                );
                return;
            }
            case value_kind::string:
                quoted(item.as_string());
                return;
            default:
                throw error(
                    error_code::conversion,
                    "value requires an explicit JSON export conversion"
                );
            }
        }
    };
}

graph_handle graph_from_expression(
    const normalized::expression_ptr& expression, graph_limits storage,
    limits execution
) {
    if (!expression)
        throw std::invalid_argument(
            "cannot construct a graph from a null expression"
        );
    graph_builder builder(storage);
    budget work(execution);
    const auto bindings = standard_environment();
    const auto lookup = names(bindings);
    const auto root = builder.declare();

    struct task {
        graph_slot slot;
        normalized::expression_ptr expression;
        size_t depth;
    };

    std::vector<task> pending { { root, expression, 0 } };
    while (!pending.empty()) {
        const auto current = pending.back();
        pending.pop_back();
        try {
            if (current.depth >= execution.depth)
                throw error(
                    error_code::resource, "graph source depth limit exceeded"
                );
            work.spend();
            if (const auto* list
                = std::get_if<normalized::list>(&current.expression->value)) {
                work.elements(list->elements.size());
                std::vector<graph_slot> slots;
                slots.reserve(list->elements.size());
                for (size_t i = 0; i < list->elements.size(); ++i)
                    slots.push_back(builder.declare());
                builder.define_list(current.slot, slots);
                for (size_t i = slots.size(); i > 0; --i)
                    pending.push_back(
                        { slots[i - 1], list->elements[i - 1],
                          current.depth + 1 }
                    );
            } else if (
                const auto* object
                = std::get_if<normalized::object>(&current.expression->value)
            ) {
                work.elements(object->entries.size());
                std::vector<std::pair<std::string, graph_slot>> slots;
                slots.reserve(object->entries.size());
                for (const auto& child : object->entries) {
                    work.bytes(child.key.text.size());
                    auto slot = builder.declare();
                    slots.emplace_back(child.key.text, slot);
                }
                builder.define_object(current.slot, slots);
                for (size_t i = slots.size(); i > 0; --i)
                    pending.push_back(
                        { slots[i - 1].second, object->entries[i - 1].value,
                          current.depth + 1 }
                    );
            } else if (reference_path(current.expression, work)) {
                builder.define_reference(current.slot, current.expression);
            } else {
                auto result = evaluate_bounded(
                    current.expression, {}, lookup, work, current.depth
                );
                builder.define_scalar(current.slot, std::move(result));
            }
        } catch (const error& failure) {
            throw failure.located(current.expression->span);
        }
    }
    return builder.publish(root);
}

std::string serialize_json(
    const value& input, reference_frame frame, const environment& bindings,
    serialization_limits options
) {
    return serializer(bindings, options).run(input, std::move(frame));
}
}
