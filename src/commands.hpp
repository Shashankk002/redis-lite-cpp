#ifndef REDIS_LITE_COMMANDS_HPP
#define REDIS_LITE_COMMANDS_HPP

#include <chrono>
#include <string>
#include <unordered_map>

#include "resp.hpp"

namespace redis_lite {

    // The whole database, for now: the values, plus a deadline for the few keys
    // that have one. A key with no entry in `expirations` never expires.
    //
    // steady_clock, not system_clock: it only ever moves forward, so a TTL is
    // unaffected if the machine's wall clock is corrected or shifts for DST.
    struct Store {
        std::unordered_map<std::string, std::string> values;
        std::unordered_map<std::string, std::chrono::steady_clock::time_point> expirations;
    };

    // Executes one parsed request and returns the reply to send back.
    // Anything invalid produces a RESP error rather than an exception, so a
    // bad request can never take the server down.
    RespValue execute_command(const RespValue& request, Store& store);

}  // namespace redis_lite

#endif  // REDIS_LITE_COMMANDS_HPP
