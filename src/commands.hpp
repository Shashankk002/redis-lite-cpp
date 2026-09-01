#ifndef REDIS_LITE_COMMANDS_HPP
#define REDIS_LITE_COMMANDS_HPP

#include <chrono>
#include <iosfwd>
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
    //
    // `log` is the append-only file that state-changing commands are recorded
    // in. Passing nullptr executes the command without recording it, which is
    // exactly what replaying the log needs.
    RespValue execute_command(const RespValue& request, Store& store, std::ofstream* log = nullptr);

}  // namespace redis_lite

#endif  // REDIS_LITE_COMMANDS_HPP
