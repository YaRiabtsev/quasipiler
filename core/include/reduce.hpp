#ifndef QPILER_REDUCE_HPP
#define QPILER_REDUCE_HPP

#include "normalized.hpp"
#include <cstdint>
#include <map>

namespace reduction {
enum class state { known, input, unresolved, residual };
enum class reason : uint32_t {
    unknown_symbol = 1U << 0U,
    input_dependency = 1U << 1U,
    unsupported_operation = 1U << 2U,
    side_effect = 1U << 3U,
    incompatible_types = 1U << 4U,
    division_by_zero = 1U << 5U,
    numeric_growth = 1U << 6U,
    expression_growth = 1U << 7U,
    invalid_literal = 1U << 8U
};
using reasons = uint32_t;

constexpr reasons flag(reason value) { return static_cast<reasons>(value); }

enum class input_type { unknown, exact_number, boolean, string };

struct input_binding {
    std::string identity;
    input_type type = input_type::unknown;
    bool operator==(const input_binding&) const = default;
};

// Bindings describe stable, immutable values in ONE lexical expression scope.
// They are never inferred from spelling or propagated through statements.
using binding = std::variant<normalized::scalar, input_binding>;
using environment = std::map<std::string, binding, std::less<>>;

struct limits {
    exact::limits numbers;
    size_t steps = 100000;
    size_t depth = 256;
};

struct result {
    normalized::expression_ptr tree;
    state kind = state::residual;
    reasons why = 0;
    std::optional<input_binding> input;
    const normalized::scalar* value() const;
};

struct site {
    source_span original;
    result value;
};

struct program_result {
    normalized::statement_ptr tree;
    reasons why = 0;
    std::vector<site> expressions;
};
enum class reporting { none, expressions };

// A failed structural budget rolls the whole expression back, so another pass
// with identical inputs/budgets cannot creep beyond the same limit.
result reduce(
    const normalized::expression_ptr& expression,
    const environment& bindings = {}, limits budget = {}
);
void dump(std::ostream& output, const result& value);
// Reduces expression sites using an empty environment. Statements, control
// flow, and deferred function bodies retain their structure; no execution or
// binding propagation occurs. One structural budget covers the entire program.
program_result reduce_program(
    const normalized::statement_ptr& program, limits budget = {},
    reporting reports = reporting::expressions
);
void dump(std::ostream& output, const program_result& value);
}
#endif
