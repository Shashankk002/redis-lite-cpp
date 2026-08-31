// Stage 0 test suite.
//
// There is no Redis functionality to test yet, so these checks only prove the
// test executable builds, runs, and reports failure through its exit code.
// Real tests arrive alongside the code they cover.

#include <iostream>
#include <string>

static int failures = 0;

static void check(bool condition, const std::string& description) {
    if (condition) {
        std::cout << "  PASS  " << description << "\n";
    } else {
        std::cout << "  FAIL  " << description << "\n";
        ++failures;
    }
}

int main() {
    std::cout << "running tests\n";

    check(1 + 1 == 2, "arithmetic works");
    check(std::string("redis").size() == 5, "std::string reports its length");

    if (failures == 0) {
        std::cout << "all tests passed\n";
        return 0;
    }

    std::cout << failures << " check(s) failed\n";
    return 1;
}
