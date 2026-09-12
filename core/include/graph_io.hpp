#ifndef QPILER_GRAPH_IO_HPP
#define QPILER_GRAPH_IO_HPP

#include "evaluate.hpp"
#include "graph.hpp"

namespace runtime {
// Construct literal lists/objects with deferred $/@ paths. Scalar expressions
// execute with the standard pure builtins and one aggregate execution budget.
graph_handle graph_from_expression(
    const normalized::expression_ptr& expression, graph_limits storage = {},
    limits execution = {}
);

struct serialization_limits {
    limits execution;
    size_t output_bytes = 1048576;
};

// Return a complete JSON document or throw. Active-path cycles fail; repeated
// DAG aliases expand independently. Exact nonintegers require explicit numeric
// export conversion; strings/keys must contain valid UTF-8.
std::string serialize_json(
    const value& input, reference_frame frame = {},
    const environment& bindings = standard_environment(),
    serialization_limits options = {}
);
}
#endif
