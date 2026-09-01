#ifndef REDIS_LITE_COMMANDS_HPP
#define REDIS_LITE_COMMANDS_HPP

#include <chrono>
#include <deque>
#include <iosfwd>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>

#include "resp.hpp"

namespace redis_lite {

    using StringValue = std::string;
    using ListValue = std::deque<std::string>;
    using HashValue = std::unordered_map<std::string, std::string>;
    using SetValue = std::unordered_set<std::string>;

    using Value = std::variant<StringValue, ListValue, HashValue, SetValue>;

    struct Store {
        std::unordered_map<std::string, Value> values;
        std::unordered_map<std::string, std::chrono::steady_clock::time_point> expirations;
    };

    RespValue execute_command(const RespValue& request, Store& store, std::ofstream* log = nullptr);

}  // namespace redis_lite

#endif  // REDIS_LITE_COMMANDS_HPP
