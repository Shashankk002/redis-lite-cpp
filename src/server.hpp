#ifndef REDIS_LITE_SERVER_HPP
#define REDIS_LITE_SERVER_HPP

#include <cstdint>
#include <string>

namespace redis_lite {

    // Replays the log, then runs the event loop on 127.0.0.1:<port> until
    // SIGINT or SIGTERM. Returns 0 on a clean shutdown, 1 if startup fails.
    int run_server(uint16_t port, const std::string& log_path);

}  // namespace redis_lite

#endif  // REDIS_LITE_SERVER_HPP
