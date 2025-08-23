![QuasiPiler Logo Light](logo/llogo.svg#gh-light-mode-only)![QuasiPiler Logo Dark](logo/dlogo.svg#gh-dark-mode-only)
## _— the Hunchback Dragon of Compilers_


[![version](https://img.shields.io/github/v/release/ninjaro/QuasiPiler?include_prereleases)](https://github.com/ninjaro/QuasiPiler/releases/latest)
[![Checks](https://github.com/ninjaro/QuasiPiler/actions/workflows/tests.yml/badge.svg)](https://github.com/ninjaro/QuasiPiler/actions/workflows/tests.yml)
[![Deploy](https://github.com/ninjaro/QuasiPiler/actions/workflows/html.yml/badge.svg)](https://github.com/ninjaro/QuasiPiler/actions/workflows/html.yml)
[![codecov](https://codecov.io/gh/ninjaro/QuasiPiler/graph/badge.svg?token=MCNEJFWMDU)](https://codecov.io/gh/ninjaro/QuasiPiler)
[![Codacy Badge](https://app.codacy.com/project/badge/Grade/940dcf5e3cf64e759ce6ad17176d31f4)](https://app.codacy.com/gh/ninjaro/QuasiPiler/dashboard?utm_source=gh&utm_medium=referral&utm_content=&utm_campaign=Badge_grade)
[![license](https://img.shields.io/github/license/ninjaro/QuasiPiler?color=e6e6e6)](https://github.com/ninjaro/QuasiPiler/blob/master/license)

> “A one-eyed transpiler is much more incomplete than a blind transpiler, for he knows what it is that’s lacking.”  
> — Victor-Marie of Gugle Inc. (1998–2017)

This repo is my sanctuary under license — it begs mercy, not stars. I’ll bell when (or if) it works.
“Documentation and Contributing” is a friendly suggestion, not a Martin Luther pinboard.

## Setup and Installation

### Requirements

* **C++20** compiler
* **cxxopts**: for command line options
* **GTest**: for unit tests
* **lcov**: for code coverage reports
* **doxygen** and **graphviz**: for generating documentation

### Building the Application

1. Build with CMake in Release mode:
    ```bash
    $ cmake -DBUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release -B build -S .
    $ cmake --build build
    ```
2. Run the Application:
    ```bash
    $ qpiler [options] <inputfile>
    ```
   * `<inputfile>`: path to your QuasiCode file

## QuasiLang

**See [QuasiLang Syntax Guide](data/readme.md) for the full syntax guide.**

**See [include](include/) and [src](src/) for implementation, and check [tests](tests/) for more examples.**

---

**See `include/frontend` and `src/frontend` for implementation, and check `tests/frontend` for more examples.**


## Documentation and Contributing

To build and run tests, enable debug mode, or generate coverage reports:

1. **Build with Debug and Coverage:**
   ```bash
   $ cmake -B build CMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=ON -DCOVERAGE=ON
   ```
2. **Generate Coverage Report and HTML:**
   ```bash
   $ cmake --build build --target coverage
   ```

For detailed documentation, see the [Documentation](https://ninjaro.github.io/QuasiPiler/doc/) and for the latest
coverage report, see [Coverage](https://ninjaro.github.io/QuasiPiler/cov/).

## Security Policy

Please report any security issues using GitHub's private vulnerability reporting
or by emailing [yaroslav.riabtsev@rwth-aachen.de](mailto:yaroslav.riabtsev@rwth-aachen.de).
See the [security policy](.github/SECURITY.md) for full details.

## License

This project is open-source and available under the MIT License.