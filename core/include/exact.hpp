#ifndef QPILER_EXACT_HPP
#define QPILER_EXACT_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace exact {
// Limits apply to canonical values AND arithmetic intermediates. The API
// rejects configurations beyond its hard ceilings before allocating integers.
struct limits {
    size_t bits = 4096;
    size_t literal_chars = 16384;
};
enum class failure {
    none,
    invalid_literal,
    growth,
    division_by_zero,
    conversion
};
enum class operation { add, subtract, multiply, divide };
struct result;
struct engine;

// Immutable canonical rational; the multiprecision backend is private.
class number {
public:
    number();
    std::string str() const;
    size_t bits() const;
    bool is_zero() const;
    bool is_one() const;
    bool is_integer() const;
    std::optional<int64_t> as_int64() const;
    std::optional<size_t> as_index(size_t length) const;
    // Correctly rounded finite binary64, independent of the floating-point
    // environment. Underflow can produce signed zero; overflow returns nullopt.
    std::optional<double> as_real() const;
    int compare(const number& other) const;
    bool operator==(const number& other) const;

private:
    struct storage;
    std::shared_ptr<const storage> data_;
    explicit number(std::shared_ptr<const storage> data);
    friend struct engine;
};

struct result {
    std::optional<number> value;
    failure error = failure::none;
};

void validate(limits budget);
result parse(std::string_view spelling, limits budget = {});
result from_real(double value, limits budget = {});
result negate(const number& value, limits budget = {});
result calculate(
    operation op, const number& left, const number& right, limits budget = {}
);
}
#endif
