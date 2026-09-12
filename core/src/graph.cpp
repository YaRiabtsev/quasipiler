#include "graph.hpp"

#include <unordered_map>
#include <unordered_set>

namespace runtime {
namespace {
    struct string_hash {
        using is_transparent = void;

        size_t operator()(std::string_view text) const {
            return std::hash<std::string_view>()(text);
        }
    };

    struct object_cell {
        std::vector<std::pair<std::string, size_t>> entries;
        std::unordered_map<std::string, size_t, string_hash, std::equal_to<>>
            lookup;
    };

    using cell = std::variant<
        std::monostate, value, std::vector<size_t>, object_cell,
        normalized::expression_ptr>;

    [[noreturn]] void invalid(const char* message) {
        throw error(error_code::invalid_reference, message);
    }

    void capacity(size_t used, size_t count, size_t maximum) {
        if (count > maximum - used)
            throw error(
                error_code::resource, "graph construction limit exceeded"
            );
    }

    size_t scalar_bytes(const value& data, exact::limits numbers) {
        switch (data.kind()) {
        case value_kind::null:
        case value_kind::boolean:
        case value_kind::real:
            return 0;
        case value_kind::integer:
        case value_kind::rational:
            if (data.as_number().bits() > numbers.bits)
                throw error(
                    error_code::resource, "graph exact number limit exceeded"
                );
            return 0;
        case value_kind::string:
            return data.as_string().size();
        default:
            throw error(
                error_code::type,
                "graph scalar must not contain collections, handles, or "
                "callables"
            );
        }
    }
}

struct graph_scope { };

struct graph_storage {
    std::vector<cell> cells;
    size_t edges = 0, bytes = 0, recipe_nodes = 0;
};

struct graph_builder_state {
    graph_limits options;
    std::shared_ptr<const graph_scope> scope
        = std::make_shared<const graph_scope>();
    graph_storage data;
};

void validate(graph_limits options) {
    exact::validate(options.numbers);
    if (options.nodes > 1000000 || options.edges > 1000000
        || options.string_bytes > 16777216 || options.recipe_nodes > 1000000)
        throw std::invalid_argument("graph limits outside supported range");
}

graph_slot::graph_slot(std::shared_ptr<const graph_scope> scope, size_t id)
    : scope_(std::move(scope))
    , id_(id) { }

graph_handle::graph_handle(
    std::shared_ptr<const graph_storage> owner, size_t id
)
    : owner_(std::move(owner))
    , id_(id) { }

bool graph_handle::valid() const {
    return owner_ && id_ < owner_->cells.size();
}

void graph_handle::check() const {
    if (!valid())
        invalid("graph handle is empty");
}

size_t graph_handle::id() const {
    check();
    return id_;
}

size_t graph_handle::node_count() const {
    check();
    return owner_->cells.size();
}

bool graph_handle::same_identity(const graph_handle& other) const {
    return valid() && other.valid() && owner_ == other.owner_
        && id_ == other.id_;
}

std::weak_ptr<const void> graph_handle::lifetime() const { return owner_; }

graph_kind graph_handle::kind() const {
    check();
    switch (owner_->cells[id_].index()) {
    case 1:
        return graph_kind::scalar;
    case 2:
        return graph_kind::list;
    case 3:
        return graph_kind::object;
    default:
        return graph_kind::reference;
    }
}

value graph_handle::scalar() const {
    check();
    if (const auto* data = std::get_if<value>(&owner_->cells[id_]))
        return *data;
    throw error(error_code::type, "expected graph scalar");
}

const normalized::expression_ptr& graph_handle::recipe() const {
    check();
    if (const auto* data
        = std::get_if<normalized::expression_ptr>(&owner_->cells[id_]))
        return *data;
    throw error(error_code::type, "expected graph reference recipe");
}

size_t graph_handle::size() const {
    check();
    if (const auto* data
        = std::get_if<std::vector<size_t>>(&owner_->cells[id_]))
        return data->size();
    if (const auto* data = std::get_if<object_cell>(&owner_->cells[id_]))
        return data->entries.size();
    throw error(error_code::type, "expected graph collection");
}

graph_handle graph_handle::at(size_t index) const {
    check();
    const auto* data = std::get_if<std::vector<size_t>>(&owner_->cells[id_]);
    if (!data)
        throw error(error_code::type, "expected graph list");
    if (index >= data->size())
        throw error(error_code::index_bounds, "list index is outside bounds");
    return graph_handle(owner_, (*data)[index]);
}

graph_handle graph_handle::member(std::string_view key) const {
    check();
    const auto* data = std::get_if<object_cell>(&owner_->cells[id_]);
    if (!data)
        throw error(error_code::type, "expected graph object");
    const auto found = data->lookup.find(key);
    if (found == data->lookup.end())
        throw error(error_code::missing_key, "object key does not exist");
    return graph_handle(owner_, data->entries[found->second].second);
}

const std::string& graph_handle::key_at(size_t index) const {
    check();
    const auto* data = std::get_if<object_cell>(&owner_->cells[id_]);
    if (!data)
        throw error(error_code::type, "expected graph object");
    if (index >= data->entries.size())
        throw error(error_code::index_bounds, "object entry is outside bounds");
    return data->entries[index].first;
}

graph_handle graph_handle::child_at(size_t index) const {
    if (kind() == graph_kind::list)
        return at(index);
    static_cast<void>(key_at(index));
    return graph_handle(
        owner_, std::get<object_cell>(owner_->cells[id_]).entries[index].second
    );
}

graph_builder::graph_builder(graph_limits options) {
    validate(options);
    state_ = std::make_unique<graph_builder_state>();
    state_->options = options;
}

graph_builder::~graph_builder() = default;
graph_builder::graph_builder(graph_builder&&) noexcept = default;
graph_builder& graph_builder::operator=(graph_builder&&) noexcept = default;

graph_builder_state& graph_builder::state() {
    if (!state_)
        invalid("graph builder has been published or moved");
    return *state_;
}

size_t graph_builder::check(graph_slot slot, bool undefined) {
    const auto& builder = state();
    if (slot.scope_ != builder.scope || slot.id_ >= builder.data.cells.size())
        invalid("graph slot is empty or belongs to another builder");
    if (undefined
        && !std::holds_alternative<std::monostate>(
            builder.data.cells[slot.id_]
        ))
        invalid("graph slot is already defined");
    return slot.id_;
}

graph_slot graph_builder::declare() {
    auto& builder = state();
    capacity(builder.data.cells.size(), 1, builder.options.nodes);
    const auto id = builder.data.cells.size();
    builder.data.cells.emplace_back();
    return graph_slot(builder.scope, id);
}

void graph_builder::define_scalar(graph_slot slot, value data) {
    const auto id = check(slot, true);
    auto& builder = state();
    const auto bytes = scalar_bytes(data, builder.options.numbers);
    capacity(builder.data.bytes, bytes, builder.options.string_bytes);
    builder.data.cells[id] = std::move(data);
    builder.data.bytes += bytes;
}

void graph_builder::define_reference(
    graph_slot slot, normalized::expression_ptr recipe
) {
    const auto id = check(slot, true);
    auto base = recipe;
    size_t path_steps = 0;
    while (base
           && !std::holds_alternative<normalized::context_reference>(
               base->value
           )) {
        capacity(0, ++path_steps, state().options.recipe_nodes);
        base = std::visit(
            [](const auto& node) -> normalized::expression_ptr {
                using T = std::decay_t<decltype(node)>;
                if constexpr (
                    std::is_same_v<T, normalized::member>
                    || std::is_same_v<T, normalized::index>
                    || std::is_same_v<T, normalized::member_selection>
                    || std::is_same_v<T, normalized::selection>
                )
                    return node.base;
                else
                    return {};
            },
            base->value
        );
    }
    if (!base)
        invalid("reference recipe must be a path anchored at $ or @");
    auto& builder = state();
    size_t count = 0;
    auto charge = [&] {
        capacity(
            builder.data.recipe_nodes + count, 1, builder.options.recipe_nodes
        );
        ++count;
    };
    std::vector<normalized::expression_ptr> expressions;
    std::vector<normalized::statement_ptr> statements;
    auto expression = [&](const normalized::expression_ptr& node) {
        charge();
        expressions.push_back(node);
    };
    auto statement = [&](const normalized::statement_ptr& node) {
        charge();
        statements.push_back(node);
    };
    expression(recipe);
    while (!expressions.empty() || !statements.empty()) {
        if (!expressions.empty()) {
            const auto node = expressions.back();
            expressions.pop_back();
            normalized::for_each_child(*node, expression, statement);
        } else {
            const auto node = statements.back();
            statements.pop_back();
            normalized::for_each_child(*node, expression, statement);
        }
    }
    builder.data.cells[id] = std::move(recipe);
    builder.data.recipe_nodes += count;
}

void graph_builder::define_list(
    graph_slot slot, std::vector<graph_slot> children
) {
    const auto id = check(slot, true);
    auto& builder = state();
    capacity(builder.data.edges, children.size(), builder.options.edges);
    std::vector<size_t> edges;
    edges.reserve(children.size());
    for (const auto& child : children)
        edges.push_back(check(child));
    builder.data.cells[id] = std::move(edges);
    builder.data.edges += children.size();
}

void graph_builder::define_object(
    graph_slot slot, std::vector<std::pair<std::string, graph_slot>> children
) {
    const auto id = check(slot, true);
    auto& builder = state();
    capacity(builder.data.edges, children.size(), builder.options.edges);
    size_t bytes = 0;
    object_cell data;
    data.entries.reserve(children.size());
    data.lookup.reserve(children.size());
    for (auto& [key, child] : children) {
        const auto target = check(child);
        capacity(
            builder.data.bytes + bytes, key.size(), builder.options.string_bytes
        );
        bytes += key.size();
        if (!data.lookup.emplace(key, data.entries.size()).second)
            throw error(
                error_code::duplicate_key, "object contains a duplicate key"
            );
        data.entries.emplace_back(std::move(key), target);
    }
    builder.data.cells[id] = std::move(data);
    builder.data.edges += children.size();
    builder.data.bytes += bytes;
}

graph_handle graph_builder::publish(graph_slot root) {
    const auto id = check(root);
    for (const auto& node : state().data.cells)
        if (std::holds_alternative<std::monostate>(node))
            invalid("graph contains an undefined slot");
    auto owner = std::make_shared<const graph_storage>(std::move(state().data));
    state_.reset();
    return graph_handle(std::move(owner), id);
}

graph_handle clone_graph(const graph_handle& root, graph_limits options) {
    validate(options);
    root.check();
    const auto& data = *root.owner_;
    capacity(0, data.cells.size(), options.nodes);
    capacity(0, data.edges, options.edges);
    capacity(0, data.bytes, options.string_bytes);
    capacity(0, data.recipe_nodes, options.recipe_nodes);
    for (const auto& node : data.cells)
        if (const auto* scalar = std::get_if<value>(&node))
            static_cast<void>(scalar_bytes(*scalar, options.numbers));
    return graph_handle(std::make_shared<const graph_storage>(data), root.id_);
}

graph_handle graph_from_value(const value& root, graph_limits options) {
    graph_builder builder(options);
    const auto first = builder.declare();
    std::vector<std::pair<graph_slot, const value*>> pending { { first,
                                                                 &root } };
    while (!pending.empty()) {
        const auto [slot, item] = pending.back();
        pending.pop_back();
        if (item->kind() == value_kind::list) {
            std::vector<graph_slot> children;
            capacity(0, item->elements().size(), options.edges);
            for (const auto& child : item->elements()) {
                auto target = builder.declare();
                children.push_back(target);
                pending.emplace_back(target, &child);
            }
            builder.define_list(slot, std::move(children));
        } else if (item->kind() == value_kind::object) {
            std::vector<std::pair<std::string, graph_slot>> children;
            capacity(0, item->entries().size(), options.edges);
            for (const auto& [key, child] : item->entries()) {
                auto target = builder.declare();
                children.emplace_back(key, target);
                pending.emplace_back(target, &child);
            }
            builder.define_object(slot, std::move(children));
        } else
            builder.define_scalar(slot, *item);
    }
    return builder.publish(first);
}

std::vector<graph_handle> walk_graph(const graph_handle& root, budget& work) {
    static_cast<void>(root.id());
    std::vector<graph_handle> result;
    std::vector<std::pair<graph_handle, size_t>> pending { { root, 0 } };
    std::unordered_set<size_t> visited;
    while (!pending.empty()) {
        auto& [node, cursor] = pending.back();
        if (cursor == 0) {
            work.spend();
            if (!visited.insert(node.id()).second) {
                pending.pop_back();
                continue;
            }
            work.elements(1);
            result.push_back(node);
        }
        if (node.kind() == graph_kind::scalar
            || node.kind() == graph_kind::reference || cursor == node.size()) {
            pending.pop_back();
            continue;
        }
        auto child = node.child_at(cursor++);
        pending.emplace_back(std::move(child), 0);
    }
    return result;
}
}
