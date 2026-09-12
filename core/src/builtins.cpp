#include "builtins.hpp"

#include <array>
#include <limits>

namespace runtime {
namespace {
    value size(std::span<const value> arguments, budget& work) {
        return numeric_literal(
            std::to_string(arguments.front().size()), work.options().numbers
        );
    }

    value
    extremum(std::span<const value> arguments, budget& work, bool maximum) {
        auto items = arguments;
        if (arguments.size() == 1
            && arguments.front().kind() == value_kind::list)
            items = arguments.front().elements();
        if (items.empty())
            throw error(
                error_code::empty_collection, "min/max requires a nonempty list"
            );
        const value* best = nullptr;
        for (const auto& item : items) {
            work.spend();
            // Unary numeric identity validates type and exact-number budget.
            static_cast<void>(unary("+", item, work.options().numbers));
            if (!best
                || binary(
                       maximum ? ">" : "<", item, *best, work.options().numbers
                )
                       .as_bool())
                best = &item;
        }
        return *best;
    }

    value minimum(std::span<const value> arguments, budget& work) {
        return extremum(arguments, work, false);
    }

    value maximum(std::span<const value> arguments, budget& work) {
        return extremum(arguments, work, true);
    }

    constexpr size_t variadic = std::numeric_limits<size_t>::max();

    struct entry {
        builtin_descriptor descriptor;
        value (*evaluate)(std::span<const value>, budget&);
    };

    constexpr std::array registry {
        entry { { builtin_id::size, "size", 1, 1, true }, size },
        entry { { builtin_id::size, "len", 1, 1, true }, size },
        entry { { builtin_id::minimum, "min", 1, variadic, true }, minimum },
        entry { { builtin_id::maximum, "max", 1, variadic, true }, maximum }
    };
    constexpr auto descriptors = [] {
        std::array<builtin_descriptor, registry.size()> result {};
        for (size_t i = 0; i < result.size(); ++i)
            result[i] = registry[i].descriptor;
        return result;
    }();
}

std::span<const builtin_descriptor> builtins() { return descriptors; }

value invoke(
    builtin_id identity, std::span<const value> arguments, budget& work
) {
    work.spend();
    for (const auto& builtin : registry) {
        if (builtin.descriptor.identity != identity)
            continue;
        if (arguments.size() < builtin.descriptor.minimum_arguments
            || arguments.size() > builtin.descriptor.maximum_arguments)
            throw error(error_code::arity, "invalid builtin argument count");
        return builtin.evaluate(arguments, work);
    }
    throw error(error_code::not_callable, "unknown builtin identity");
}
}
