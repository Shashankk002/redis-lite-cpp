#ifndef REDIS_LITE_PERSISTENCE_HPP
#define REDIS_LITE_PERSISTENCE_HPP

#include <fstream>
#include <initializer_list>
#include <string>
#include <string_view>

#include "commands.hpp"

namespace redis_lite {

    bool open_log(const std::string& path, std::ofstream& log);

    void log_command(std::ofstream* log,
                     std::string_view verb,
                     std::initializer_list<std::string_view> arguments);


    void log_expire(std::ofstream* log, const std::string& key, long long seconds_from_now);

    bool replay_log(const std::string& path, Store& store, std::string& error);

}  // namespace redis_lite

#endif  // REDIS_LITE_PERSISTENCE_HPP
