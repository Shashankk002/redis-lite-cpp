#ifndef REDIS_LITE_SERVER_HPP
#define REDIS_LITE_SERVER_HPP

#include <cstdint>

namespace redis_lite {

// Runs the TCP server on 127.0.0.1:<port> until Ctrl+C.
//
// Blocking and sequential: one client is handled from connect to disconnect
// before the next one is accepted. Returns 0 on a clean shutdown, 1 on error.
int run_server(uint16_t port);

}  // namespace redis_lite

#endif  // REDIS_LITE_SERVER_HPP
