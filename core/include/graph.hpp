#ifndef QPILER_GRAPH_HPP
#define QPILER_GRAPH_HPP

#include "normalized.hpp"
#include "runtime.hpp"

namespace runtime {
struct graph_storage;
struct graph_builder_state;
struct graph_scope;

struct graph_limits {
    size_t nodes = 100000;
    size_t edges = 100000;
    size_t string_bytes = 1048576;
    size_t recipe_nodes = 100000;
    exact::limits numbers { 65536, 65536 };
};

void validate(graph_limits options);
enum class graph_kind { scalar, list, object, reference };

// A slot belongs to one builder. It carries no graph ownership and cannot be
// forged from a numeric ID; declarations support forward and cyclic edges.
class graph_slot {
public:
    graph_slot() = default;

    size_t id() const { return id_; }

private:
    std::shared_ptr<const graph_scope> scope_;
    size_t id_ = 0;
    graph_slot(std::shared_ptr<const graph_scope> scope, size_t id);
    friend class graph_builder;
};

// Copying a handle preserves identity and owns the complete immutable arena.
// Internal cells contain only local IDs and safe scalar payloads.
class graph_handle {
public:
    graph_handle() = default;
    bool valid() const;
    size_t id() const;
    size_t node_count() const;
    graph_kind kind() const;
    bool same_identity(const graph_handle& other) const;
    std::weak_ptr<const void> lifetime() const;
    value scalar() const;
    const normalized::expression_ptr& recipe() const;
    size_t size() const;
    graph_handle at(size_t index) const;
    graph_handle member(std::string_view key) const;
    const std::string& key_at(size_t index) const;
    graph_handle child_at(size_t index) const;

private:
    std::shared_ptr<const graph_storage> owner_;
    size_t id_ = 0;
    graph_handle(std::shared_ptr<const graph_storage> owner, size_t id);
    void check() const;
    friend class graph_builder;
    friend graph_handle clone_graph(const graph_handle&, graph_limits);
};

class graph_builder {
public:
    explicit graph_builder(graph_limits options = {});
    ~graph_builder();
    graph_builder(const graph_builder&) = delete;
    graph_builder& operator=(const graph_builder&) = delete;
    graph_builder(graph_builder&&) noexcept;
    graph_builder& operator=(graph_builder&&) noexcept;

    graph_slot declare();
    void define_scalar(graph_slot slot, value data);
    void define_reference(graph_slot slot, normalized::expression_ptr recipe);
    void define_list(graph_slot slot, std::vector<graph_slot> children);
    void define_object(
        graph_slot slot,
        std::vector<std::pair<std::string, graph_slot>> children
    );
    graph_handle publish(graph_slot root);

private:
    std::unique_ptr<graph_builder_state> state_;
    graph_builder_state& state();
    size_t check(graph_slot slot, bool undefined = false);
};

// Copy the entire owner: a new identity domain, with all aliases and local IDs
// preserved. Import of ordinary values expands each occurrence independently.
graph_handle clone_graph(const graph_handle& root, graph_limits options = {});
graph_handle graph_from_value(const value& root, graph_limits options = {});

// Deterministic depth-first first-visit order; cycles and repeated aliases are
// visited once. Work and returned handles are charged to the caller's budget.
std::vector<graph_handle> walk_graph(const graph_handle& root, budget& work);
}
#endif
