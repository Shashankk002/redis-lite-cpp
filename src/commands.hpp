#ifndef REDIS_LITE_COMMANDS_HPP
#define REDIS_LITE_COMMANDS_HPP

#include <string>
#include <unordered_map>

#include "resp.hpp"

namespace redis_lite {
    using Store = std::unordered_map<std::string, std::string>;

    // Executes one parsed request and returns the reply to send back.
    RespValue execute_command(const RespValue& request, Store& store);

}  // namespace redis_lite

#endif  // REDIS_LITE_COMMANDS_HPP
