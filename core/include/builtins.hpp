#ifndef QPILER_BUILTINS_HPP
#define QPILER_BUILTINS_HPP

#include "runtime.hpp"

namespace runtime {
struct builtin_descriptor {
    builtin_id identity;
    std::string_view name;
    size_t minimum_arguments, maximum_arguments;
    bool pure;
};

std::span<const builtin_descriptor> builtins();
value invoke(
    builtin_id identity, std::span<const value> arguments, budget& work
);
}
#endif
