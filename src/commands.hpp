#ifndef REDIS_LITE_COMMANDS_HPP
#define REDIS_LITE_COMMANDS_HPP

#include <string>
#include <unordered_map>

#include "resp.hpp"

namespace redis_lite {

    // The whole database, for now. Keys and values are both plain strings.
    using Store = std::unordered_map<std::string, std::string>;

    // Executes one parsed request and returns the reply to send back.
    // Anything invalid produces a RESP error rather than an exception, so a
    // bad request can never take the server down.
    RespValue execute_command(const RespValue& request, Store& store);

}  // namespace redis_lite

#endif  // REDIS_LITE_COMMANDS_HPP
