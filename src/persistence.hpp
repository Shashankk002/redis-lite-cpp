#ifndef REDIS_LITE_PERSISTENCE_HPP
#define REDIS_LITE_PERSISTENCE_HPP

#include <fstream>
#include <string>

#include "commands.hpp"

namespace redis_lite {

    // The append-only log: every state-changing command is appended to a file so
    // the store can be rebuilt on the next start.
    //
    // Format -- one record per line, every key and value length-prefixed:
    //
    //     SET <keylen> <key> <valuelen> <value>\n
    //     DEL <keylen> <key>\n
    //     EXPIRE <keylen> <key> <unix-deadline-seconds>\n
    //
    // The length prefixes are what make arbitrary bytes safe: keys and values
    // may contain spaces, newlines or NULs, because the reader takes exactly
    // that many bytes instead of looking for a delimiter.
    //
    // Expirations are written as an absolute Unix timestamp. A steady_clock
    // value could not be used: it counts from an arbitrary origin (usually
    // boot) and means nothing in the next process.

    // Opens `path` for appending, creating it if needed.
    bool open_log(const std::string& path, std::ofstream& log);

    // Append one record. A null `log` writes nothing -- that is what lets replay
    // reuse the ordinary command path without growing the file it is reading.
    void log_set(std::ofstream* log, const std::string& key, const std::string& value);
    void log_del(std::ofstream* log, const std::string& key);
    void log_expire(std::ofstream* log, const std::string& key, long long seconds_from_now);

    // Rebuilds `store` from `path`. A missing or empty file is not an error.
    // On malformed data this returns false, sets `error`, and leaves `store`
    // untouched rather than half-applied.
    bool replay_log(const std::string& path, Store& store, std::string& error);

}  // namespace redis_lite

#endif  // REDIS_LITE_PERSISTENCE_HPP
