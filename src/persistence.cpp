#include "persistence.hpp"

#include <chrono>
#include <istream>
#include <vector>

#include "resp.hpp"

namespace redis_lite {

    namespace {

        // Wall-clock seconds since the Unix epoch. Unlike steady_clock this
        // survives a restart, which is the whole point for persisted deadlines.
        long long unix_now() {
            const auto now = std::chrono::system_clock::now().time_since_epoch();
            return std::chrono::duration_cast<std::chrono::seconds>(now).count();
        }

        void write_field(std::ofstream& log, const std::string& text) {
            log << text.size() << ' ' << text;
        }

        // Reads one "<length> <bytes>" field.
        bool read_field(std::istream& in, std::string& out) {
            long long length = -1;
            if (!(in >> length) || length < 0) {
                return false;
            }
            if (in.get() != ' ') {
                return false;
            }

            out.assign(static_cast<std::size_t>(length), '\0');
            if (length > 0 && !in.read(&out[0], length)) {
                return false;
            }
            return true;
        }

        // Builds a request array, so replay runs through exactly the same
        // execute_command() that a network client would reach.
        RespValue as_request(const std::vector<std::string>& words) {
            std::vector<RespValue> elements;
            for (const std::string& word : words) {
                elements.push_back(make_bulk_string(word));
            }
            return make_array(elements);
        }

        std::string record_error(long long record_number, const std::string& detail) {
            return "malformed persistence record #" + std::to_string(record_number) +
                   ": " + detail;
        }

    }  // namespace

    bool open_log(const std::string& path, std::ofstream& log) {
        log.open(path, std::ios::out | std::ios::app | std::ios::binary);
        return log.is_open();
    }

    void log_set(std::ofstream* log, const std::string& key, const std::string& value) {
        if (log == nullptr) {
            return;  // replaying: the record is already in the file
        }

        *log << "SET ";
        write_field(*log, key);
        *log << ' ';
        write_field(*log, value);
        *log << '\n';

        // flush() pushes our buffers into the OS. It is not fsync(): the bytes
        // may still be sitting in the kernel's page cache if the machine loses
        // power. See the README for the distinction.
        log->flush();
    }

    void log_del(std::ofstream* log, const std::string& key) {
        if (log == nullptr) {
            return;
        }

        *log << "DEL ";
        write_field(*log, key);
        *log << '\n';
        log->flush();
    }

    void log_expire(std::ofstream* log, const std::string& key, long long seconds_from_now) {
        if (log == nullptr) {
            return;
        }

        // Stored as an absolute wall-clock deadline, so a restart can work out
        // how much life the key has left.
        *log << "EXPIRE ";
        write_field(*log, key);
        *log << ' ' << (unix_now() + seconds_from_now) << '\n';
        log->flush();
    }

    bool replay_log(const std::string& path, Store& store, std::string& error) {
        std::ifstream in(path, std::ios::in | std::ios::binary);
        if (!in.is_open()) {
            return true;  // no file yet: a fresh server starts empty
        }

        // Replay into a scratch store so a malformed file cannot leave the real
        // one half-populated.
        Store loaded;
        long long record_number = 0;

        while (true) {
            std::string verb;
            if (!(in >> verb)) {
                break;  // end of file
            }
            ++record_number;

            if (in.get() != ' ') {
                error = record_error(record_number, "expected a space after " + verb);
                return false;
            }

            std::string key;
            if (!read_field(in, key)) {
                error = record_error(record_number, "bad key field");
                return false;
            }

            if (verb == "SET") {
                if (in.get() != ' ') {
                    error = record_error(record_number, "expected a space before the value");
                    return false;
                }

                std::string value;
                if (!read_field(in, value)) {
                    error = record_error(record_number, "bad value field");
                    return false;
                }
                if (in.get() != '\n') {
                    error = record_error(record_number, "record does not end with a newline");
                    return false;
                }

                execute_command(as_request({"SET", key, value}), loaded, nullptr);

            } else if (verb == "DEL") {
                if (in.get() != '\n') {
                    error = record_error(record_number, "record does not end with a newline");
                    return false;
                }

                execute_command(as_request({"DEL", key}), loaded, nullptr);

            } else if (verb == "EXPIRE") {
                if (in.get() != ' ') {
                    error = record_error(record_number, "expected a space before the deadline");
                    return false;
                }

                long long deadline = 0;
                if (!(in >> deadline)) {
                    error = record_error(record_number, "bad deadline");
                    return false;
                }
                if (in.get() != '\n') {
                    error = record_error(record_number, "record does not end with a newline");
                    return false;
                }

                // Turn the stored wall-clock deadline back into a duration the
                // in-memory (steady_clock) expiration understands.
                const long long remaining = deadline - unix_now();
                if (remaining <= 0) {
                    // The key's life ran out while the server was down.
                    execute_command(as_request({"DEL", key}), loaded, nullptr);
                } else {
                    execute_command(as_request({"EXPIRE", key, std::to_string(remaining)}),
                                    loaded, nullptr);
                }

            } else {
                error = record_error(record_number, "unknown record type '" + verb + "'");
                return false;
            }
        }

        store = std::move(loaded);
        return true;
    }

}  // namespace redis_lite
