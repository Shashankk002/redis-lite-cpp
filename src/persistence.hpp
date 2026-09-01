#ifndef REDIS_LITE_PERSISTENCE_HPP
#define REDIS_LITE_PERSISTENCE_HPP

#include <fstream>
#include <string>
#include <vector>

#include "commands.hpp"

namespace redis_lite {

    bool open_log(const std::string& path, std::ofstream& log);

    void log_command(std::ofstream* log,
                     const std::string& verb,
                     const std::vector<std::string>& arguments);


    void log_expire(std::ofstream* log, const std::string& key, long long seconds_from_now);

    bool replay_log(const std::string& path, Store& store, std::string& error);

}  // namespace redis_lite

#endif  // REDIS_LITE_PERSISTENCE_HPP
