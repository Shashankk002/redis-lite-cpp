// A deliberately tiny test framework.
//
// Redis-Lite is a from-scratch systems project, so the test harness is
// dependency-free too: register cases with TEST(), assert with CHECK/CHECK_EQ,
// and let tests/test_main.cpp run them. Swap in a real framework later if the
// suite outgrows this.
#ifndef REDIS_LITE_TESTS_TESTING_HPP
#define REDIS_LITE_TESTS_TESTING_HPP

#include <iostream>
#include <string>
#include <vector>

namespace redis_lite::testing {

using TestFn = void (*)();

struct TestCase {
    const char* name;
    TestFn fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> cases;
    return cases;
}

// Failures inside the currently running test case.
inline int& current_failures() {
    static int failures = 0;
    return failures;
}

struct Registrar {
    Registrar(const char* name, TestFn fn) { registry().push_back({name, fn}); }
};

inline void report_failure(const char* file, int line, const std::string& detail) {
    ++current_failures();
    std::cerr << "    FAIL " << file << ':' << line << ": " << detail << '\n';
}

// Runs every registered case and returns a process exit code.
inline int run_all() {
    int failed_cases = 0;

    std::cout << "running " << registry().size() << " test(s)\n";
    for (const TestCase& test : registry()) {
        current_failures() = 0;
        std::cout << "  " << test.name << '\n';
        test.fn();
        if (current_failures() > 0) {
            ++failed_cases;
        }
    }

    if (failed_cases == 0) {
        std::cout << "all tests passed\n";
        return 0;
    }
    std::cout << failed_cases << " test(s) failed\n";
    return 1;
}

}  // namespace redis_lite::testing

#define REDIS_LITE_TEST_CONCAT_INNER(a, b) a##b
#define REDIS_LITE_TEST_CONCAT(a, b) REDIS_LITE_TEST_CONCAT_INNER(a, b)

// Defines and registers a test case: TEST(name) { ... }
#define TEST(name)                                                     \
    static void name();                                                \
    static const ::redis_lite::testing::Registrar                      \
        REDIS_LITE_TEST_CONCAT(name, _registrar){#name, &name};        \
    static void name()

#define CHECK(expr)                                                    \
    do {                                                               \
        if (!(expr)) {                                                 \
            ::redis_lite::testing::report_failure(                     \
                __FILE__, __LINE__, "CHECK(" #expr ") is false");      \
        }                                                              \
    } while (false)

#define CHECK_EQ(lhs, rhs)                                             \
    do {                                                               \
        if (!((lhs) == (rhs))) {                                       \
            ::redis_lite::testing::report_failure(                     \
                __FILE__, __LINE__,                                    \
                "CHECK_EQ(" #lhs ", " #rhs ") is false");              \
        }                                                              \
    } while (false)

#endif  // REDIS_LITE_TESTS_TESTING_HPP
