#ifndef QPILER_EVALUATE_HPP
#define QPILER_EVALUATE_HPP

#include "builtins.hpp"
#include "normalized.hpp"
#include <map>

namespace runtime {
using environment = std::map<std::string, value, std::less<>>;
environment standard_environment();
using resolver = std::function<std::optional<value>(std::string_view)>;

struct reference_frame {
    std::optional<value> root, current;
};

struct evaluation {
    value output;
    size_t steps, elements, string_bytes;
};

// Executes exactly one expression. A custom environment is complete; callers
// opt into builtins via standard_environment() and may shadow or alias them.
evaluation evaluate(
    const normalized::expression_ptr& expression,
    const environment& bindings = standard_environment(), limits options = {}
);
// A host resolver is called once for each evaluated identifier, in source
// evaluation order. Missing names have no implicit builtin fallback.
evaluation evaluate_with(
    const normalized::expression_ptr& expression, const resolver& bindings,
    limits options = {}
);
evaluation evaluate_in(
    const normalized::expression_ptr& expression, reference_frame frame,
    const environment& bindings = standard_environment(), limits options = {}
);
evaluation evaluate_with_context(
    const normalized::expression_ptr& expression, reference_frame frame,
    const resolver& bindings, limits options = {}
);
// Resolve stored graph recipes under one caller-owned work budget. Useful for
// bounded host traversal/serialization; collection contents are not expanded.
value resolve_references(
    value input, reference_frame frame, const resolver& bindings, budget& work,
    size_t enclosing_depth = 0
);
// Embedding operations preserve their already consumed depth as well as work.
value evaluate_bounded(
    const normalized::expression_ptr& expression, reference_frame frame,
    const resolver& bindings, budget& work, size_t enclosing_depth = 0
);
}
#endif
