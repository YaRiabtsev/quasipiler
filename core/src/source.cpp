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

#include "source.hpp"

#include <algorithm>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <utility>

source::source(std::string data, std::string name)
    : data_(std::move(data))
    , name_(std::move(name)) {
    if (data_.size()
        > static_cast<size_t>(std::numeric_limits<std::streamoff>::max())) {
        throw std::length_error("source exceeds byte offset range");
    }
    for (size_t i = 0; i < data_.size(); ++i) {
        if (data_[i] == '\n') {
            line_starts_.push_back(static_cast<std::streamoff>(i + 1));
        }
    }
    // All representable source positions must fit the existing public API.
    if (line_starts_.size() - 1
        > static_cast<size_t>(std::numeric_limits<int>::max())) {
        throw std::length_error("source exceeds line number range");
    }
    for (size_t i = 0; i < line_starts_.size(); ++i) {
        const auto end
            = i + 1 < line_starts_.size() ? line_starts_[i + 1] - 1 : size();
        if (end - line_starts_[i] > std::numeric_limits<int>::max()) {
            throw std::length_error("source exceeds column number range");
        }
    }
}

source_ptr source::from_memory(std::string data, std::string name) {
    return source_ptr(new source(std::move(data), std::move(name)));
}

source_ptr source::from_file(
    const std::filesystem::path& path, const std::streamsize chunk_size
) {
    if (chunk_size <= 0) {
        throw std::invalid_argument("source read chunk size must be positive");
    }
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        throw std::invalid_argument("cannot open file: " + path.string());
    }
    std::string data;
    std::string chunk(static_cast<size_t>(chunk_size), '\0');
    while (file) {
        file.read(chunk.data(), chunk_size);
        data.append(chunk.data(), static_cast<size_t>(file.gcount()));
    }
    if (file.bad() || !file.eof()) {
        throw std::runtime_error("cannot read file: " + path.string());
    }
    return from_memory(std::move(data), path.string());
}

std::string_view source::text() const noexcept { return data_; }

const std::string& source::name() const noexcept { return name_; }

std::streamoff source::size() const noexcept {
    return static_cast<std::streamoff>(data_.size());
}

position source::position_at(const std::streamoff offset) const {
    if (offset < 0 || offset > size()) {
        throw std::out_of_range("source position is out of range");
    }
    const auto upper
        = std::upper_bound(line_starts_.begin(), line_starts_.end(), offset);
    const auto index = static_cast<size_t>(upper - line_starts_.begin() - 1);
    return { offset, static_cast<int>(index),
             static_cast<int>(offset - line_starts_[index]) };
}

source_span::source_span(
    source_ptr owner, const std::streamoff begin, const std::streamoff end
)
    : owner_(std::move(owner)) {
    if (!owner_) {
        throw std::invalid_argument("source span requires an owner");
    }
    if (begin < 0 || end < begin || end > owner_->size()) {
        throw std::out_of_range("source span is out of range");
    }
    begin_ = owner_->position_at(begin);
    end_ = owner_->position_at(end);
}

const source_ptr& source_span::owner() const noexcept { return owner_; }

position source_span::begin() const noexcept { return begin_; }

position source_span::end() const noexcept { return end_; }

std::string_view source_span::text() const noexcept {
    return owner_->text().substr(
        static_cast<size_t>(begin_.offset),
        static_cast<size_t>(end_.offset - begin_.offset)
    );
}
