// SPDX-License-Identifier: MIT
// A ~120-line test framework. A third-party one would be more capable, but this project's
// policy is that `shared/` carries no external dependency, and the test suite is the place that
// policy is easiest to quietly break.
#ifndef OBSN_TEST_HPP
#define OBSN_TEST_HPP

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace obsn_test {

struct TestCase {
    std::string suite;
    std::string name;
    std::function<void()> body;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

struct Registrar {
    Registrar(const char* suite, const char* name, std::function<void()> body) {
        registry().push_back({suite, name, std::move(body)});
    }
};

struct Failure {
    std::string message;
};

inline void fail(const std::string& file, int line, const std::string& message) {
    std::ostringstream out;
    out << file << ":" << line << ": " << message;
    throw Failure{out.str()};
}

/// Streams a value when it is streamable, and degrades to a placeholder when it is not, so a
/// CHECK_EQ on a domain type without an operator<< still compiles and still reports usefully.
template <typename T, typename = void>
struct Show {
    static std::string to_string(const T&) { return "<value>"; }
};

template <typename T>
struct Show<T, std::void_t<decltype(std::declval<std::ostringstream&>()
                                    << std::declval<const T&>())>> {
    static std::string to_string(const T& value) {
        std::ostringstream out;
        out << value;
        return out.str();
    }
};

template <typename T>
std::string show(const T& value) {
    return Show<T>::to_string(value);
}
inline std::string show(bool value) { return value ? "true" : "false"; }
inline std::string show(const std::string& value) { return "\"" + value + "\""; }

inline int run(int argc, char** argv) {
    const std::string filter = argc > 1 ? argv[1] : "";
    int passed = 0;
    std::vector<std::string> failures;

    for (const TestCase& test : registry()) {
        const std::string full = test.suite + "." + test.name;
        if (!filter.empty() && full.find(filter) == std::string::npos) continue;
        try {
            test.body();
            ++passed;
            std::printf("  ok   %s\n", full.c_str());
        } catch (const Failure& f) {
            failures.push_back(full + "\n       " + f.message);
            std::printf("  FAIL %s\n       %s\n", full.c_str(), f.message.c_str());
        } catch (const std::exception& e) {
            failures.push_back(full + "\n       unexpected exception: " + e.what());
            std::printf("  FAIL %s (exception: %s)\n", full.c_str(), e.what());
        }
    }

    std::printf("\n%d passed, %zu failed\n", passed, failures.size());
    return failures.empty() ? 0 : 1;
}

}  // namespace obsn_test

#define OBSN_CONCAT_INNER(a, b) a##b
#define OBSN_CONCAT(a, b) OBSN_CONCAT_INNER(a, b)

#define TEST(suite, name)                                                               \
    static void OBSN_CONCAT(tsro_test_body_, __LINE__)();                               \
    static ::obsn_test::Registrar OBSN_CONCAT(tsro_test_reg_, __LINE__)(                \
        #suite, #name, &OBSN_CONCAT(tsro_test_body_, __LINE__));                        \
    static void OBSN_CONCAT(tsro_test_body_, __LINE__)()

#define CHECK(cond)                                                                     \
    do {                                                                                \
        if (!(cond)) ::obsn_test::fail(__FILE__, __LINE__, "expected: " #cond);          \
    } while (false)

#define CHECK_EQ(actual, expected)                                                      \
    do {                                                                                \
        const auto& obsn_a = (actual);                                                  \
        const auto& obsn_b = (expected);                                                \
        if (!(obsn_a == obsn_b)) {                                                       \
            ::obsn_test::fail(__FILE__, __LINE__,                                        \
                              std::string(#actual " == " #expected "\n         actual:   ") + \
                                  ::obsn_test::show(obsn_a) + "\n         expected: " +  \
                                  ::obsn_test::show(obsn_b));                            \
        }                                                                                \
    } while (false)

#define CHECK_NE(actual, expected)                                                      \
    do {                                                                                \
        if ((actual) == (expected))                                                      \
            ::obsn_test::fail(__FILE__, __LINE__, #actual " should differ from " #expected); \
    } while (false)

#define CHECK_NEAR(actual, expected, tolerance)                                         \
    do {                                                                                \
        const double obsn_a = static_cast<double>(actual);                              \
        const double obsn_b = static_cast<double>(expected);                            \
        const double obsn_t = static_cast<double>(tolerance);                           \
        if (!((obsn_a - obsn_b) <= obsn_t && (obsn_b - obsn_a) <= obsn_t)) {             \
            ::obsn_test::fail(__FILE__, __LINE__,                                        \
                              std::string(#actual " ~= " #expected "\n         actual:   ") + \
                                  ::obsn_test::show(obsn_a) + "\n         expected: " +  \
                                  ::obsn_test::show(obsn_b));                            \
        }                                                                                \
    } while (false)

#endif  // OBSN_TEST_HPP
