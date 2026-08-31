#include <iostream>

#include "redis_lite/version.hpp"

int main() {
    std::cout << "Redis-Lite server starting...\n";
    std::cout << "version " << redis_lite::kVersion << '\n';
    return 0;
}
