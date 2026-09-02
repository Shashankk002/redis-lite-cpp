#include "persistence.hpp"

#include <charconv>
#include <chrono>
#include <istream>

#include "resp.hpp"

namespace redis_lite {

    namespace {

        struct RecordShape {
            const char* verb;
            int arguments;
        };

        const RecordShape kRecordShapes[] = {
            {"SET",   2},
            {"DEL",   1},
            {"LPUSH", 2},
            {"RPUSH", 2},
            {"LPOP",  1},
            {"RPOP",  1},
            {"HSET",  3},
            {"HDEL",  2},
            {"SADD",  2},
            {"SREM",  2},
        };

        int arguments_for(const std::string& verb) {
            for (const RecordShape& shape : kRecordShapes) {
                if (verb == shape.verb) {
                    return shape.arguments;
                }
            }
            return -1;
        }

        long long unix_now() {
            const auto now = std::chrono::system_clock::now().time_since_epoch();
            return std::chrono::duration_cast<std::chrono::seconds>(now).count();
        }

        void append_number(std::string& record, long long number) {
            char digits[24];
            const auto end = std::to_chars(digits, digits + sizeof(digits), number).ptr;
            record.append(digits, static_cast<std::size_t>(end - digits));
        }

        void append_field(std::string& record, std::string_view text) {
            append_number(record, static_cast<long long>(text.size()));
            record += ' ';
            record.append(text);
        }

        void write_field(std::ofstream& log, const std::string& text) {
            log << text.size() << ' ' << text;
        }

        bool read_field(std::istream& in, std::streamoff limit, std::string& out) {
            long long length = -1;
            if (!(in >> length) || length < 0) {
                return false;
            }
            if (in.get() != ' ') {
                return false;
            }

            
            const std::streamoff here = in.tellg();
            if (here < 0 || length > limit - here) {
                return false;
            }

            out.assign(static_cast<std::size_t>(length), '\0');
            if (length > 0 && !in.read(&out[0], length)) {
                return false;
            }
            return true;
        }

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

    void log_command(std::ofstream* log,
                     std::string_view verb,
                     std::initializer_list<std::string_view> arguments) {
        if (log == nullptr) {
            return;  // replaying: the record is already in the file
        }

        // One buffer and one write: measurably cheaper than a chain of stream
        // insertions, which pay for locale-aware formatting each time.
        std::string record(verb);
        for (std::string_view argument : arguments) {
            record += ' ';
            append_field(record, argument);
        }
        record += '\n';

        log->write(record.data(), static_cast<std::streamsize>(record.size()));
        log->flush();
    }

    void log_expire(std::ofstream* log, const std::string& key, long long seconds_from_now) {
        if (log == nullptr) {
            return;
        }

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

        in.seekg(0, std::ios::end);
        const std::streamoff file_size = in.tellg();
        in.seekg(0, std::ios::beg);
        if (file_size < 0) {
            error = "cannot determine the size of " + path;
            return false;
        }

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
            if (!read_field(in, file_size, key)) {
                error = record_error(record_number, "bad key field");
                return false;
            }

            if (verb == "EXPIRE") {
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

                const long long remaining = deadline - unix_now();
                if (remaining <= 0) {
                    // The key's life ran out while the server was down.
                    execute_command(as_request({"DEL", key}), loaded, nullptr);
                } else {
                    execute_command(as_request({"EXPIRE", key, std::to_string(remaining)}),
                                    loaded, nullptr);
                }
                continue;
            }

            const int expected = arguments_for(verb);
            if (expected < 0) {
                error = record_error(record_number, "unknown record type '" + verb + "'");
                return false;
            }

            std::vector<std::string> words{verb, key};
            for (int i = 1; i < expected; ++i) {
                if (in.get() != ' ') {
                    error = record_error(record_number, "expected a space before argument " +
                                                            std::to_string(i + 1));
                    return false;
                }

                std::string argument;
                if (!read_field(in, file_size, argument)) {
                    error = record_error(record_number,
                                         "bad argument " + std::to_string(i + 1));
                    return false;
                }
                words.push_back(argument);
            }

            if (in.get() != '\n') {
                error = record_error(record_number, "record does not end with a newline");
                return false;
            }

            execute_command(as_request(words), loaded, nullptr);
        }

        store = std::move(loaded);
        return true;
    }

}  // namespace redis_lite
