#include "runtime.hpp"
#include "graph.hpp"

#include <algorithm>
#include <bit>
#include <cfenv>
#include <cmath>
#include <unordered_map>

namespace runtime {
namespace {
    std::string diagnostic(
        error_code code, const std::string& message,
        const std::optional<source_span>& location
    ) {
        std::string prefix;
        if (location)
            prefix = location->owner()->name() + ':'
                + std::to_string(location->begin().line + 1) + ':'
                + std::to_string(location->begin().column + 1) + ": ";
        return prefix + std::string(name(code)) + ": " + message;
    }

    [[noreturn]] void wrong_type(std::string_view expected) {
        throw error(error_code::type, "expected " + std::string(expected));
    }

    void
    consume(size_t& used, size_t count, size_t limit, const char* message) {
        if (count > limit - used)
            throw error(error_code::resource, message);
        used += count;
    }

    value exact_result(exact::result result) {
        if (result.value)
            return value(*result.value);
        switch (result.error) {
        case exact::failure::growth:
            throw error(error_code::resource, "exact number limit exceeded");
        case exact::failure::division_by_zero:
            throw error(error_code::division_by_zero, "division by zero");
        default:
            throw error(error_code::conversion, "invalid numeric conversion");
        }
    }

    bool exact_kind(value_kind kind) {
        return kind == value_kind::integer || kind == value_kind::rational;
    }

    void check_number(const value& value, exact::limits limits) {
        if (exact_kind(value.kind()) && value.as_number().bits() > limits.bits)
            throw error(error_code::resource, "exact number limit exceeded");
    }

    void check_rounding() {
        if (std::fegetround() != FE_TONEAREST)
            throw error(
                error_code::numeric_environment,
                "binary64 arithmetic requires round-to-nearest"
            );
        // Detect environments that flush subnormal operands/results to zero.
        volatile double tiny = std::bit_cast<double>(uint64_t(1));
        volatile double one = 1.0;
        const double probe = tiny * one;
        if (std::bit_cast<uint64_t>(probe) != 1)
            throw error(
                error_code::numeric_environment,
                "binary64 arithmetic requires subnormal support"
            );
    }

    struct string_hash {
        using is_transparent = void;

        size_t operator()(std::string_view key) const {
            return std::hash<std::string_view>()(key);
        }
    };
}

struct list_storage {
    std::vector<value> items;
};

struct object_storage {
    std::vector<std::pair<std::string, value>> items;
    std::unordered_map<std::string, size_t, string_hash, std::equal_to<>>
        lookup;
};

std::string_view name(error_code code) {
    switch (code) {
    case error_code::type:
        return "type_error";
    case error_code::arity:
        return "arity_error";
    case error_code::missing_key:
        return "missing_key";
    case error_code::index_bounds:
        return "index_bounds";
    case error_code::division_by_zero:
        return "division_by_zero";
    case error_code::conversion:
        return "conversion_error";
    case error_code::unsupported_operation:
        return "unsupported_operation";
    case error_code::resource:
        return "resource_exhausted";
    case error_code::unknown_name:
        return "unknown_name";
    case error_code::not_callable:
        return "not_callable";
    case error_code::duplicate_key:
        return "duplicate_key";
    case error_code::numeric_environment:
        return "numeric_environment";
    case error_code::empty_collection:
        return "empty_collection";
    case error_code::invalid_reference:
        return "invalid_reference";
    case error_code::reference_cycle:
        return "reference_cycle";
    case error_code::serialization_cycle:
        return "serialization_cycle";
    }
    return "runtime_error";
}

error::error(
    error_code code, std::string message, std::optional<source_span> location
)
    : std::runtime_error(diagnostic(code, message, location))
    , code_(code)
    , message_(std::move(message))
    , location_(std::move(location)) { }

error error::located(source_span span) const {
    return location_ ? *this : error(code_, message_, std::move(span));
}

void validate(limits options) {
    exact::validate(options.numbers);
    if (options.steps > 1000000 || options.depth > 512
        || options.elements > 1000000 || options.string_bytes > 16777216)
        throw std::invalid_argument("runtime limits outside supported range");
}

budget::budget(limits options)
    : options_(options) {
    validate(options);
}

void budget::spend(size_t count) {
    consume(steps_, count, options_.steps, "execution step limit exceeded");
}

void budget::elements(size_t count) {
    consume(
        elements_, count, options_.elements, "collection element limit exceeded"
    );
}

void budget::bytes(size_t count) {
    consume(bytes_, count, options_.string_bytes, "string byte limit exceeded");
}

value::value(value&& other) noexcept
    : data_(std::move(other.data_))
    , depth_(other.depth_) {
    other.data_ = std::monostate {};
    other.depth_ = 1;
}

value& value::operator=(value&& other) noexcept {
    if (this != &other) {
        data_ = std::move(other.data_);
        depth_ = other.depth_;
        other.data_ = std::monostate {};
        other.depth_ = 1;
    }
    return *this;
}

value::value(bool data)
    : data_(data) { }

value::value(exact::number data)
    : data_(std::move(data)) { }

value::value(double data)
    : data_(data) {
    if (!std::isfinite(data))
        throw error(error_code::conversion, "runtime real must be finite");
}

value::value(std::string data)
    : data_(std::move(data)) {
    if (std::get<std::string>(data_).size() > 16777216)
        throw error(error_code::resource, "string exceeds storage limit");
}

value value::integer(int64_t data) {
    return numeric_literal(std::to_string(data));
}

value value::list(std::vector<value> data) {
    if (data.size() > 1000000)
        throw error(error_code::resource, "list exceeds storage limit");
    value result;
    for (const auto& item : data)
        result.depth_ = std::max(result.depth_, item.depth() + 1);
    if (result.depth_ > 512)
        throw error(error_code::resource, "value depth limit exceeded");
    result.data_ = std::make_shared<const list_storage>(list_storage {
        std::move(data) });
    return result;
}

value value::object(std::vector<std::pair<std::string, value>> data) {
    if (data.size() > 1000000)
        throw error(error_code::resource, "object exceeds storage limit");
    value result;
    object_storage object;
    object.lookup.reserve(data.size());
    for (size_t i = 0; i < data.size(); ++i) {
        const auto& [key, item] = data[i];
        if (key.size() > 16777216)
            throw error(
                error_code::resource, "object key exceeds storage limit"
            );
        if (!object.lookup.emplace(key, i).second)
            throw error(
                error_code::duplicate_key, "object contains a duplicate key"
            );
        result.depth_ = std::max(result.depth_, item.depth() + 1);
    }
    if (result.depth_ > 512)
        throw error(error_code::resource, "value depth limit exceeded");
    object.items = std::move(data);
    result.data_ = std::make_shared<const object_storage>(std::move(object));
    return result;
}

value value::builtin(builtin_id id) {
    value result;
    result.data_ = id;
    return result;
}

value value::graph(graph_handle node) {
    if (!node.valid())
        throw error(error_code::invalid_reference, "graph handle is empty");
    value result;
    result.data_ = std::make_shared<const graph_handle>(std::move(node));
    return result;
}

const graph_handle& value::as_graph() const {
    if (const auto* node
        = std::get_if<std::shared_ptr<const graph_handle>>(&data_))
        return **node;
    wrong_type("graph handle");
}

value_kind value::kind() const {
    switch (data_.index()) {
    case 0:
        return value_kind::null;
    case 1:
        return value_kind::boolean;
    case 2:
        return std::get<exact::number>(data_).is_integer()
            ? value_kind::integer
            : value_kind::rational;
    case 3:
        return value_kind::real;
    case 4:
        return value_kind::string;
    case 5:
        return value_kind::list;
    case 6:
        return value_kind::object;
    case 7:
        return value_kind::builtin;
    default:
        return value_kind::graph;
    }
}

bool value::as_bool() const {
    if (const auto* value = std::get_if<bool>(&data_))
        return *value;
    wrong_type("boolean");
}

const exact::number& value::as_number() const {
    if (const auto* value = std::get_if<exact::number>(&data_))
        return *value;
    wrong_type("exact number");
}

double value::as_real() const {
    if (const auto* value = std::get_if<double>(&data_))
        return *value;
    wrong_type("runtime real");
}

const std::string& value::as_string() const {
    if (const auto* value = std::get_if<std::string>(&data_))
        return *value;
    wrong_type("string");
}

const std::vector<value>& value::elements() const {
    if (const auto* value
        = std::get_if<std::shared_ptr<const list_storage>>(&data_))
        return (*value)->items;
    wrong_type("list");
}

const std::vector<std::pair<std::string, value>>& value::entries() const {
    if (const auto* value
        = std::get_if<std::shared_ptr<const object_storage>>(&data_))
        return (*value)->items;
    wrong_type("object");
}

builtin_id value::as_builtin() const {
    if (const auto* value = std::get_if<builtin_id>(&data_))
        return *value;
    throw error(error_code::not_callable, "value is not callable");
}

size_t value::size() const {
    switch (kind()) {
    case value_kind::list:
        return elements().size();
    case value_kind::object:
        return entries().size();
    case value_kind::string:
        return as_string().size();
    case value_kind::graph:
        return as_graph().size();
    default:
        wrong_type("string, list, or object");
    }
}

value value::member(std::string_view key) const {
    if (kind() == value_kind::graph)
        return value::graph(as_graph().member(key));
    if (const auto* object
        = std::get_if<std::shared_ptr<const object_storage>>(&data_)) {
        const auto found = (*object)->lookup.find(key);
        if (found == (*object)->lookup.end())
            throw error(error_code::missing_key, "object key does not exist");
        return (*object)->items[found->second].second;
    }
    wrong_type("object");
}

value numeric_literal(std::string_view spelling, exact::limits limits) {
    return exact_result(exact::parse(spelling, limits));
}

value to_real(const value& input) {
    if (input.kind() == value_kind::real)
        return input;
    const auto converted = input.as_number().as_real();
    if (!converted)
        throw error(error_code::conversion, "exact number overflows binary64");
    return value(*converted);
}

value to_exact(const value& input, exact::limits limits) {
    exact::validate(limits);
    if (exact_kind(input.kind())) {
        check_number(input, limits);
        return input;
    }
    return exact_result(exact::from_real(input.as_real(), limits));
}

value to_integer(const value& input, exact::limits limits) {
    const auto exact = to_exact(input, limits);
    if (!exact.as_number().is_integer())
        throw error(
            error_code::conversion,
            "integer conversion requires an integral value"
        );
    return exact;
}

int64_t to_int64(const value& input) {
    const auto converted = to_integer(input).as_number().as_int64();
    if (!converted)
        throw error(error_code::conversion, "integer is outside int64 range");
    return *converted;
}

value unary(std::string_view op, const value& operand, exact::limits limits) {
    exact::validate(limits);
    check_number(operand, limits);
    if (op == "!")
        return value(!operand.as_bool());
    if (op != "+" && op != "-")
        throw error(
            error_code::unsupported_operation, "unsupported unary operator"
        );
    if (operand.kind() == value_kind::real)
        return op == "+" ? operand : value(-operand.as_real());
    const auto& number = operand.as_number();
    return op == "+" ? operand : exact_result(exact::negate(number, limits));
}

value binary(
    std::string_view op, const value& left, const value& right,
    exact::limits limits
) {
    exact::validate(limits);
    check_number(left, limits);
    check_number(right, limits);
    if (op == "&&" || op == "||") {
        const bool a = left.as_bool(), b = right.as_bool();
        return value(op == "&&" ? a && b : a || b);
    }
    const bool arithmetic = op == "+" || op == "-" || op == "*" || op == "/";
    const bool equality = op == "==" || op == "!=";
    const bool ordering = op == "<" || op == "<=" || op == ">" || op == ">=";
    if (!arithmetic && !equality && !ordering)
        throw error(
            error_code::unsupported_operation, "unsupported binary operator"
        );
    int order = 0;
    if (exact_kind(left.kind()) && exact_kind(right.kind())) {
        if (arithmetic) {
            const auto operation = op == "+" ? exact::operation::add
                : op == "-"                  ? exact::operation::subtract
                : op == "*"                  ? exact::operation::multiply
                                             : exact::operation::divide;
            return exact_result(
                exact::calculate(
                    operation, left.as_number(), right.as_number(), limits
                )
            );
        }
        order = left.as_number().compare(right.as_number());
    } else if (
        left.kind() == value_kind::real && right.kind() == value_kind::real
    ) {
        check_rounding();
        const double a = left.as_real(), b = right.as_real();
        if (arithmetic) {
            if (op == "/" && std::fpclassify(b) == FP_ZERO)
                throw error(error_code::division_by_zero, "division by zero");
            const double result = op == "+" ? a + b
                : op == "-"                 ? a - b
                : op == "*"                 ? a * b
                                            : a / b;
            if (!std::isfinite(result))
                throw error(
                    error_code::conversion, "binary64 arithmetic overflow"
                );
            return value(result);
        }
        order = a < b ? -1 : a > b ? 1 : 0;
    } else if (equality && left.kind() == right.kind()) {
        switch (left.kind()) {
        case value_kind::null:
            break;
        case value_kind::boolean:
            order = left.as_bool() == right.as_bool() ? 0 : 1;
            break;
        case value_kind::string:
            order = left.as_string().compare(right.as_string());
            break;
        default:
            throw error(
                error_code::unsupported_operation,
                "collection and callable equality is unsupported"
            );
        }
    } else
        wrong_type(
            arithmetic || ordering ? "numbers in the same numeric domain"
                                   : "scalars of the same type"
        );
    return value(
        op == "=="       ? order == 0
            : op == "!=" ? order != 0
            : op == "<"  ? order < 0
            : op == "<=" ? order <= 0
            : op == ">"  ? order > 0
                         : order >= 0
    );
}

value access(const value& base, const value& key) {
    if (base.kind() == value_kind::graph) {
        const auto& node = base.as_graph();
        if (node.kind() == graph_kind::object)
            return value::graph(node.member(key.as_string()));
        if (node.kind() != graph_kind::list)
            wrong_type("list or object");
        if (!exact_kind(key.kind()) || !key.as_number().is_integer())
            wrong_type("exact integer index");
        const auto index = key.as_number().as_index(node.size());
        if (!index)
            throw error(
                error_code::index_bounds, "list index is outside bounds"
            );
        return value::graph(node.at(*index));
    }
    if (base.kind() == value_kind::object)
        return base.member(key.as_string());
    if (base.kind() != value_kind::list)
        wrong_type("list or object");
    if (!exact_kind(key.kind()) || !key.as_number().is_integer())
        wrong_type("exact integer index");
    const auto index = key.as_number().as_index(base.elements().size());
    if (!index)
        throw error(error_code::index_bounds, "list index is outside bounds");
    return base.elements()[*index];
}
}
