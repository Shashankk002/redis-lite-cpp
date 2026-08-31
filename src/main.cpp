#include <iostream>

#include "server.hpp"

int main() {
    std::cout << "Redis-Lite server starting...\n";
    return redis_lite::run_server(6379);
}
