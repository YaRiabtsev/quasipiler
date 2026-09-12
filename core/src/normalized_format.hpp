#ifndef NORMALIZED_FORMAT_HPP
#define NORMALIZED_FORMAT_HPP

#include <string>
#include <string_view>

namespace normalized::detail {
inline std::string quoted_text(std::string_view text) {
    std::string result = "\"";
    for (const char character : text) {
        const auto byte = static_cast<unsigned char>(character);
        switch (byte) {
        case '"':
            result += "\\\"";
            break;
        case '\\':
            result += "\\\\";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
        case '\b':
            result += "\\b";
            break;
        case '\f':
            result += "\\f";
            break;
        default:
            if (byte < 0x20) {
                constexpr char hex[] = "0123456789abcdef";
                result += "\\u00";
                result += hex[byte >> 4];
                result += hex[byte & 15];
            } else
                result += static_cast<char>(byte);
        }
    }
    return result + '"';
}
}

#endif // NORMALIZED_FORMAT_HPP
