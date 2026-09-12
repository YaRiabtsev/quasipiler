/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2025 Yaroslav Riabtsev <yaroslav.riabtsev@rwth-aachen.de>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef SOURCE_HPP
#define SOURCE_HPP

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

/// Absolute byte offset and zero-based line / byte column in a source.
struct position {
    std::streamoff offset {};
    int line {};
    int column {};
    bool operator==(const position&) const = default;
};

class source;
using source_ptr = std::shared_ptr<const source>;

/// Immutable snapshot. Sharing a source never shares a reader cursor.
class source final {
public:
    static source_ptr
    from_memory(std::string data, std::string name = "<memory>");
    static source_ptr from_file(
        const std::filesystem::path& path, std::streamsize chunk_size = 4096
    );

    std::string_view text() const noexcept;
    const std::string& name() const noexcept;
    std::streamoff size() const noexcept;
    position position_at(std::streamoff offset) const;

private:
    source(std::string data, std::string name);
    const std::string data_;
    const std::string name_;
    std::vector<std::streamoff> line_starts_ { 0 };
};

/// Validated half-open byte interval that retains its immutable source.
class source_span final {
public:
    source_span(source_ptr owner, std::streamoff begin, std::streamoff end);
    const source_ptr& owner() const noexcept;
    position begin() const noexcept;
    position end() const noexcept;
    std::string_view text() const noexcept;

private:
    source_ptr owner_;
    position begin_;
    position end_;
};

#endif // SOURCE_HPP
