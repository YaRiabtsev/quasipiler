#ifndef QPILER_RUNTIME_HPP
#define QPILER_RUNTIME_HPP

#include "exact.hpp"
#include "source.hpp"
#include <span>
#include <stdexcept>
#include <variant>
#include <vector>

namespace runtime {
enum class error_code {
    type,
    arity,
    missing_key,
    index_bounds,
    division_by_zero,
    conversion,
    unsupported_operation,
    resource,
    unknown_name,
    not_callable,
    duplicate_key,
    numeric_environment,
    empty_collection,
    invalid_reference,
    reference_cycle,
    serialization_cycle
};
std::string_view name(error_code code);

class error final : public std::runtime_error {
public:
    error(
        error_code code, std::string message,
        std::optional<source_span> location = {}
    );

    error_code code() const { return code_; }

    const std::optional<source_span>& location() const { return location_; }

    error located(source_span span) const;

private:
    error_code code_;
    std::string message_;
    std::optional<source_span> location_;
};

struct limits {
    exact::limits numbers { 65536, 65536 };
    size_t steps = 100000;
    size_t depth = 256;
    size_t elements = 100000;
    size_t string_bytes = 1048576;
};

void validate(limits options);

// Invocation-owned counters: node visits and builtin scans share one budget.
class budget {
public:
    explicit budget(limits options = {});

    const limits& options() const { return options_; }

    void spend(size_t count = 1);
    void elements(size_t count);
    void bytes(size_t count);

    size_t steps_used() const { return steps_; }

    size_t elements_used() const { return elements_; }

    size_t bytes_used() const { return bytes_; }

private:
    limits options_;
    size_t steps_ = 0, elements_ = 0, bytes_ = 0;
};

enum class builtin_id { size, minimum, maximum };
enum class value_kind {
    null,
    boolean,
    integer,
    rational,
    real,
    string,
    list,
    object,
    builtin,
    graph
};
struct list_storage;
struct object_storage;
class graph_handle;

// Public operations never mutate collection storage or expose its identity.
class value {
public:
    value() = default;
    value(const value&) = default;
    value& operator=(const value&) = default;
    value(value&& other) noexcept;
    value& operator=(value&& other) noexcept;
    explicit value(bool data);
    explicit value(exact::number data);
    explicit value(double data);
    explicit value(std::string data);

    explicit value(const char* data)
        : value(std::string(data)) { }

    static value integer(int64_t data);
    static value list(std::vector<value> data);
    static value object(std::vector<std::pair<std::string, value>> data);
    static value builtin(builtin_id id);
    static value graph(graph_handle node);
    const graph_handle& as_graph() const;

    value_kind kind() const;

    size_t depth() const { return depth_; }

    bool as_bool() const;
    const exact::number& as_number() const;
    double as_real() const;
    const std::string& as_string() const;
    const std::vector<value>& elements() const;
    const std::vector<std::pair<std::string, value>>& entries() const;
    builtin_id as_builtin() const;
    size_t size() const;
    value member(std::string_view key) const;

private:
    using payload = std::variant<
        std::monostate, bool, exact::number, double, std::string,
        std::shared_ptr<const list_storage>,
        std::shared_ptr<const object_storage>, builtin_id,
        std::shared_ptr<const graph_handle>>;
    payload data_;
    size_t depth_ = 1;
};

value numeric_literal(
    std::string_view spelling, exact::limits limits = { 65536, 65536 }
);
value to_real(const value& input);
value to_exact(const value& input, exact::limits limits = { 65536, 65536 });
value to_integer(const value& input, exact::limits limits = { 65536, 65536 });
int64_t to_int64(const value& input);
value unary(
    std::string_view op, const value& operand,
    exact::limits limits = { 65536, 65536 }
);
value binary(
    std::string_view op, const value& left, const value& right,
    exact::limits limits = { 65536, 65536 }
);
value access(const value& base, const value& key);
}
#endif
