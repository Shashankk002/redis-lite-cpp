#include <iostream>
#include <string>

#include "server.hpp"

int main(int argc, char* argv[]) {
    // Optional argument: where to keep the append-only log.
    const std::string log_path = (argc > 1) ? argv[1] : "redis-lite.aof";

    std::cout << "Redis-Lite server starting...\n";
    return redis_lite::run_server(6379, log_path);
}
