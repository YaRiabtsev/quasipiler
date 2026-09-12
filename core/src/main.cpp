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

#include <cxxopts.hpp>
#include <fstream>
#include <iostream>

#include "normalized.hpp"
#include "reduce.hpp"

int main(const int argc, char* argv[]) {
    std::filesystem::path path;
    bool dumps = false;
    try {
        cxxopts::Options options(
            "QuasiPiler", "the Hunchback Dragon of Compilers"
        );
        options
            .add_options()("i,input", "Input file", cxxopts::value<std::filesystem::path>(path))("d,dump", "Write compiler stages to <input>.dump/", cxxopts::value<bool>(dumps))(
                "h,help", "Show help"
            );
        options.parse_positional({ "input" });
        const auto result = options.parse(argc, argv);
        if (result.count("help")) {
            std::cout << options.help() << "\n";
            return 0;
        }
        if (path.empty() || !is_regular_file(path)) {
            std::cerr << "input file is required.\n";
            return 1;
        }

        const auto input = source::from_file(path);
        const source_span span(input, 0, input->size());
        const auto program = normalized::normalize_program(span);
        const auto reduced = reduction::reduce_program(
            program, {},
            dumps ? reduction::reporting::expressions
                  : reduction::reporting::none
        );
        if (dumps) {
            auto directory = path;
            directory += ".dump";
            std::filesystem::create_directories(directory);
            std::ofstream tokens, ast, reduced_ast, reasons;
            tokens.exceptions(std::ios::failbit | std::ios::badbit);
            ast.exceptions(std::ios::failbit | std::ios::badbit);
            reduced_ast.exceptions(std::ios::failbit | std::ios::badbit);
            reasons.exceptions(std::ios::failbit | std::ios::badbit);
            tokens.open(
                directory / "00.tokens", std::ios::binary | std::ios::trunc
            );
            ast.open(
                directory / "01.normalized", std::ios::binary | std::ios::trunc
            );
            reduced_ast.open(
                directory / "02.reduced", std::ios::binary | std::ios::trunc
            );
            reasons.open(
                directory / "03.reduction", std::ios::binary | std::ios::trunc
            );
            normalized::dump_tokens(tokens, span);
            normalized::dump(ast, program);
            normalized::dump(reduced_ast, reduced.tree);
            reduction::dump(reasons, reduced);
            tokens.close();
            ast.close();
            reduced_ast.close();
            reasons.close();
        }
        return 0;
    } catch (const cxxopts::exceptions::exception& error) {
        std::cerr << "error parsing options: " << error.what() << "\n";
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << "\n";
    }
    return 1;
}
