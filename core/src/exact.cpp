#include "exact.hpp"

#include <algorithm>
#include <bit>
#include <boost/multiprecision/cpp_int.hpp>
#include <limits>
#include <stdexcept>
#include <utility>

namespace exact {
using integer = boost::multiprecision::cpp_int;
static_assert(
    sizeof(double) == sizeof(uint64_t) && std::numeric_limits<double>::is_iec559
    && std::numeric_limits<double>::digits == 53
);

namespace {
    integer magnitude(const integer& value) {
        return value < 0 ? -value : value;
    }

    size_t width(const integer& value) {
        return value == 0
            ? 0
            : static_cast<size_t>(boost::multiprecision::msb(magnitude(value)))
                + 1;
    }

    integer gcd(integer left, integer right) {
        left = magnitude(left);
        right = magnitude(right);
        while (right != 0) {
            integer remainder = left % right;
            left = std::move(right);
            right = std::move(remainder);
        }
        return left;
    }

    bool digit(char ch) { return ch >= '0' && ch <= '9'; }

    struct exhausted { };
}

struct number::storage {
    integer numerator, denominator;
};

struct engine {
    limits budget;
    integer maximum;

    explicit engine(limits value)
        : budget(value) {
        validate(budget);
        boost::multiprecision::bit_set(
            maximum, static_cast<unsigned>(budget.bits)
        );
        --maximum;
    }

    integer multiply(const integer& left, const integer& right) const {
        if (right != 0 && magnitude(left) > maximum / magnitude(right))
            throw exhausted {};
        return left * right;
    }

    integer add(const integer& left, const integer& right) const {
        if ((left < 0) == (right < 0)
            && magnitude(left) > maximum - magnitude(right))
            throw exhausted {};
        return left + right;
    }

    integer power10(size_t exponent) const {
        // 10^n needs more than n bits; reject before any power construction.
        if (exponent >= budget.bits)
            throw exhausted {};
        integer result = 1, base = 10;
        while (exponent != 0) {
            if ((exponent & 1U) != 0)
                result = multiply(result, base);
            exponent >>= 1U;
            if (exponent != 0)
                base = multiply(base, base);
        }
        return result;
    }

    number make(integer numerator, integer denominator = 1) const {
        if (denominator < 0) {
            numerator = -numerator;
            denominator = -denominator;
        }
        const integer divisor = gcd(numerator, denominator);
        numerator /= divisor;
        denominator /= divisor;
        if (width(numerator) > budget.bits || width(denominator) > budget.bits)
            throw exhausted {};
        return number(
            std::make_shared<const number::storage>(number::storage {
                std::move(numerator), std::move(denominator) })
        );
    }

    result read(std::string_view spelling) const {
        if (spelling.size() > budget.literal_chars)
            return { {}, failure::growth };
        size_t cursor = 0;
        bool negative = false;
        if (cursor < spelling.size()
            && (spelling[cursor] == '+' || spelling[cursor] == '-'))
            negative = spelling[cursor++] == '-';
        const size_t start = cursor;
        while (cursor < spelling.size() && digit(spelling[cursor]))
            ++cursor;
        if (cursor == start || (cursor - start > 1 && spelling[start] == '0'))
            return { {}, failure::invalid_literal };
        std::string digits(spelling.substr(start, cursor - start));
        size_t fractional = 0;
        if (cursor < spelling.size() && spelling[cursor] == '.') {
            const size_t first = ++cursor;
            while (cursor < spelling.size() && digit(spelling[cursor]))
                ++cursor;
            fractional = cursor - first;
            if (fractional == 0)
                return { {}, failure::invalid_literal };
            digits.append(spelling.substr(first, fractional));
        }
        bool exponent_negative = false, exponent_exhausted = false;
        size_t exponent = 0;
        if (cursor < spelling.size()
            && (spelling[cursor] == 'e' || spelling[cursor] == 'E')) {
            ++cursor;
            if (cursor < spelling.size()
                && (spelling[cursor] == '+' || spelling[cursor] == '-'))
                exponent_negative = spelling[cursor++] == '-';
            const size_t first = cursor;
            // Enough headroom for cancellation against every mantissa digit.
            const size_t ceiling = budget.literal_chars + budget.bits;
            while (cursor < spelling.size() && digit(spelling[cursor])) {
                const size_t next
                    = static_cast<size_t>(spelling[cursor++] - '0');
                if (next > ceiling || exponent > (ceiling - next) / 10)
                    exponent_exhausted = true;
                else if (!exponent_exhausted)
                    exponent = exponent * 10 + next;
            }
            if (cursor == first)
                return { {}, failure::invalid_literal };
        }
        if (cursor != spelling.size())
            return { {}, failure::invalid_literal };
        const size_t first_nonzero = digits.find_first_not_of('0');
        if (first_nonzero == std::string::npos)
            return { make(0) };
        if (exponent_exhausted)
            return { {}, failure::growth };
        digits.erase(0, first_nonzero);
        size_t trailing = 0;
        while (digits.back() == '0') {
            digits.pop_back();
            ++trailing;
        }
        auto scale = static_cast<std::ptrdiff_t>(exponent);
        if (exponent_negative)
            scale = -scale;
        scale += static_cast<std::ptrdiff_t>(trailing)
            - static_cast<std::ptrdiff_t>(fractional);
        integer numerator = 0;
        for (char ch : digits)
            numerator = add(multiply(numerator, 10), integer(ch - '0'));
        integer denominator = 1;
        if (scale >= 0)
            numerator
                = multiply(numerator, power10(static_cast<size_t>(scale)));
        else {
            // Cancel factors before constructing the denominator: e.g. 0.5
            // can be parsed in three bits even though 10 does not fit.
            size_t twos = static_cast<size_t>(-scale), fives = twos;
            while (twos != 0 && numerator % 2 == 0) {
                numerator /= 2;
                --twos;
            }
            while (fives != 0 && numerator % 5 == 0) {
                numerator /= 5;
                --fives;
            }
            if (twos >= budget.bits || fives >= budget.bits)
                throw exhausted {};
            denominator <<= twos;
            for (size_t i = 0; i < fives; ++i)
                denominator = multiply(denominator, 5);
        }
        return { make(negative ? -numerator : numerator, denominator) };
    }

    result apply(operation op, const number& left, const number& right) const {
        if (left.bits() > budget.bits || right.bits() > budget.bits)
            return { {}, failure::growth };
        integer a = left.data_->numerator, b = left.data_->denominator;
        integer c = right.data_->numerator, d = right.data_->denominator;
        if (op == operation::divide) {
            if (c == 0)
                return { {}, failure::division_by_zero };
            std::swap(c, d);
        }
        if (op == operation::multiply || op == operation::divide) {
            const integer first = gcd(a, d), second = gcd(c, b);
            a /= first;
            d /= first;
            c /= second;
            b /= second;
            return { make(multiply(a, c), multiply(b, d)) };
        }
        const integer common = gcd(b, d);
        const integer left_scale = d / common, right_scale = b / common;
        if (op == operation::subtract)
            c = -c;
        const integer sum
            = add(multiply(a, left_scale), multiply(c, right_scale));
        const integer cancel = gcd(sum, common);
        return { make(sum / cancel, multiply(right_scale, d / cancel)) };
    }

    result read_real(double value) const {
        const auto encoding = std::bit_cast<uint64_t>(value);
        const auto exponent = static_cast<int>((encoding >> 52U) & 0x7ffU);
        if (exponent == 0x7ff)
            return { {}, failure::conversion };
        uint64_t significand = encoding & ((uint64_t(1) << 52U) - 1);
        if (exponent != 0)
            significand |= uint64_t(1) << 52U;
        if (significand == 0)
            return { make(0) };
        int scale = exponent == 0 ? -1074 : exponent - 1023 - 52;
        const int trailing = std::countr_zero(significand);
        significand >>= static_cast<unsigned>(trailing);
        scale += trailing;
        integer numerator = significand, denominator = 1;
        if (scale >= 0) {
            if (width(numerator) + static_cast<size_t>(scale) > budget.bits)
                throw exhausted {};
            numerator <<= static_cast<unsigned>(scale);
        } else {
            if (static_cast<size_t>(-scale) >= budget.bits)
                throw exhausted {};
            denominator = 0;
            boost::multiprecision::bit_set(
                denominator, static_cast<unsigned>(-scale)
            );
        }
        if ((encoding >> 63U) != 0)
            numerator = -numerator;
        return { make(std::move(numerator), std::move(denominator)) };
    }
};

void validate(limits budget) {
    if (budget.bits == 0 || budget.bits > 65536 || budget.literal_chars == 0
        || budget.literal_chars > 1048576)
        throw std::invalid_argument(
            "exact arithmetic limits outside supported range"
        );
}

number::number()
    : data_(std::make_shared<const storage>(storage { 0, 1 })) { }

number::number(std::shared_ptr<const storage> data)
    : data_(std::move(data)) { }

std::string number::str() const {
    const auto numerator = data_->numerator.str();
    return data_->denominator == 1 ? numerator
                                   : numerator + '/' + data_->denominator.str();
}

size_t number::bits() const {
    return std::max(width(data_->numerator), width(data_->denominator));
}

bool number::is_zero() const { return data_->numerator == 0; }

bool number::is_one() const { return data_->numerator == data_->denominator; }

bool number::is_integer() const { return data_->denominator == 1; }

std::optional<int64_t> number::as_int64() const {
    if (!is_integer() || data_->numerator < std::numeric_limits<int64_t>::min()
        || data_->numerator > std::numeric_limits<int64_t>::max())
        return {};
    return data_->numerator.convert_to<int64_t>();
}

std::optional<size_t> number::as_index(size_t length) const {
    if (!is_integer())
        return {};
    const auto& index = data_->numerator;
    if (index >= 0)
        return index < length ? std::optional(index.convert_to<size_t>())
                              : std::nullopt;
    const integer distance = -index;
    if (distance > length)
        return {};
    return length - distance.convert_to<size_t>();
}

std::optional<double> number::as_real() const {
    integer numerator = magnitude(data_->numerator),
            denominator = data_->denominator;
    const uint64_t sign = data_->numerator < 0 ? uint64_t(1) << 63U : 0;
    if (numerator == 0)
        return 0.0;
    int exponent = static_cast<int>(width(numerator))
        - static_cast<int>(width(denominator));
    if (exponent >= 0) {
        if (numerator < (denominator << static_cast<unsigned>(exponent)))
            --exponent;
    } else if ((numerator << static_cast<unsigned>(-exponent)) < denominator)
        --exponent;
    if (exponent > 1023)
        return {};
    if (exponent < -1075)
        return std::bit_cast<double>(sign);
    const bool subnormal = exponent < -1022;
    const int scale = subnormal ? 1074 : 52 - exponent;
    // Scratch integers are bounded by the stored number width plus 1074 bits.
    // No numerator/denominator is first rounded to a finite machine value.
    if (scale >= 0)
        numerator <<= static_cast<unsigned>(scale);
    else
        denominator <<= static_cast<unsigned>(-scale);
    integer quotient = numerator / denominator;
    const integer remainder = numerator % denominator;
    const integer complement = denominator - remainder;
    if (remainder > complement
        || (remainder == complement && (quotient & 1) != 0))
        ++quotient;
    uint64_t mantissa = quotient.convert_to<uint64_t>();
    if (subnormal)
        return std::bit_cast<double>(sign | mantissa);
    if (mantissa == (uint64_t(1) << 53U)) {
        mantissa >>= 1U;
        if (++exponent > 1023)
            return {};
    }
    const uint64_t encoded_exponent = static_cast<uint64_t>(exponent + 1023)
        << 52U;
    return std::bit_cast<double>(
        sign | encoded_exponent | (mantissa - (uint64_t(1) << 52U))
    );
}

bool number::operator==(const number& other) const {
    return data_->numerator == other.data_->numerator
        && data_->denominator == other.data_->denominator;
}

int number::compare(const number& other) const {
    const bool negative = data_->numerator < 0;
    if (negative != (other.data_->numerator < 0))
        return negative ? -1 : 1;
    // Continued fractions compare without cross-products or integer growth.
    integer a = magnitude(data_->numerator), b = data_->denominator;
    integer c = magnitude(other.data_->numerator), d = other.data_->denominator;
    int direction = negative ? -1 : 1;
    while (true) {
        const integer left = a / b, right = c / d;
        if (left != right)
            return (left < right ? -1 : 1) * direction;
        integer ar = a % b, cr = c % d;
        if (ar == 0 || cr == 0)
            return (ar == cr ? 0 : ar == 0 ? -1 : 1) * direction;
        a = std::move(b);
        b = std::move(ar);
        c = std::move(d);
        d = std::move(cr);
        direction = -direction;
    }
}

result parse(std::string_view spelling, limits budget) {
    try {
        return engine(budget).read(spelling);
    } catch (const exhausted&) {
        return { {}, failure::growth };
    }
}

result from_real(double value, limits budget) {
    try {
        return engine(budget).read_real(value);
    } catch (const exhausted&) {
        return { {}, failure::growth };
    }
}

result negate(const number& value, limits budget) {
    const engine arithmetic(budget);
    if (value.bits() > budget.bits)
        return { {}, failure::growth };
    return arithmetic.apply(operation::subtract, number(), value);
}

result calculate(
    operation op, const number& left, const number& right, limits budget
) {
    try {
        return engine(budget).apply(op, left, right);
    } catch (const exhausted&) {
        return { {}, failure::growth };
    }
}
}
