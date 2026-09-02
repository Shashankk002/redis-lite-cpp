#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

#include "server.hpp"

int main(int argc, char* argv[]) {
    // Optional arguments: where to keep the append-only log, then which port.
    const std::string log_path = (argc > 1) ? argv[1] : "redis-lite.aof";

    uint16_t port = 6379;
    if (argc > 2) {
        char* end = nullptr;
        const long parsed = std::strtol(argv[2], &end, 10);
        if (end == argv[2] || *end != '\0' || parsed < 1 || parsed > 65535) {
            std::cerr << "usage: redis-lite-server [log-path] [port]\n";
            return 1;
        }
        port = static_cast<uint16_t>(parsed);
    }

    std::cout << "Redis-Lite server starting...\n";
    return redis_lite::run_server(port, log_path);
}
