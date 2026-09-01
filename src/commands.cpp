#include "commands.hpp"

#include <cctype>
#include <charconv>
#include <fstream>

#include "persistence.hpp"

namespace redis_lite {

    namespace {

        using Clock = std::chrono::steady_clock;

        // Command names are case-insensitive: ping, PING and PiNg are one command.
        std::string to_upper(const std::string& text) {
            std::string out = text;
            for (char& c : out) {
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }
            return out;
        }

        // Strict integer conversion: the whole text must be a number.
        bool to_integer(const std::string& text, long long& out) {
            const std::from_chars_result result =
                std::from_chars(text.data(), text.data() + text.size(), out);

            return result.ec == std::errc() && result.ptr == text.data() + text.size();
        }

        RespValue wrong_arity(const std::string& name) {
            return make_error("ERR wrong number of arguments for '" + name + "' command");
        }

        // A key and its deadline always go away together.
        void remove_key(Store& store, const std::string& key) {
            store.values.erase(key);
            store.expirations.erase(key);
        }

        // Lazy expiration: drop the key if its deadline has passed. Called at the
        // start of every command that touches a key, so an expired key looks
        // exactly like a key that was never there.
        void expire_if_due(Store& store, const std::string& key) {
            const auto deadline = store.expirations.find(key);
            if (deadline == store.expirations.end()) {
                return;  // no expiration set
            }
            if (Clock::now() < deadline->second) {
                return;  // not yet
            }
            remove_key(store, key);
        }

    }  // namespace

    RespValue execute_command(const RespValue& request, Store& store, std::ofstream* log) {
        // Clients send commands as an array of bulk strings; anything else is
        // a client bug, not a command.
        if (request.type != RespType::Array || request.is_null || request.elements.empty()) {
            return make_error("ERR expected a non-empty array of bulk strings");
        }
        for (const RespValue& element : request.elements) {
            if (element.type != RespType::BulkString || element.is_null) {
                return make_error("ERR expected a non-empty array of bulk strings");
            }
        }

        const std::string name = to_upper(request.elements[0].string);
        const std::size_t argc = request.elements.size();

        if (name == "PING") {
            if (argc != 1) {
                return wrong_arity("ping");
            }
            return make_simple_string("PONG");
        }

        if (name == "SET") {
            if (argc != 3) {
                return wrong_arity("set");
            }
            const std::string& key = request.elements[1].string;
            store.values[key] = request.elements[2].string;
            store.expirations.erase(key);  // a fresh SET clears any old TTL

            // Replaying this record re-creates both effects: the value is set
            // and any earlier expiration is dropped.
            log_set(log, key, request.elements[2].string);
            return make_simple_string("OK");
        }

        if (name == "GET") {
            if (argc != 2) {
                return wrong_arity("get");
            }
            const std::string& key = request.elements[1].string;
            expire_if_due(store, key);

            const auto found = store.values.find(key);
            if (found == store.values.end()) {
                return make_null_bulk_string();  // $-1: the key does not exist
            }
            return make_bulk_string(found->second);
        }

        if (name == "DEL") {
            if (argc != 2) {
                return wrong_arity("del");
            }
            const std::string& key = request.elements[1].string;
            expire_if_due(store, key);

            const bool existed = store.values.count(key) > 0;
            remove_key(store, key);

            // Only a DEL that removed something is worth recording; deleting a
            // key that was not there changes no state.
            if (existed) {
                log_del(log, key);
            }
            return make_integer(existed ? 1 : 0);
        }

        if (name == "EXPIRE") {
            if (argc != 3) {
                return wrong_arity("expire");
            }

            long long seconds = 0;
            if (!to_integer(request.elements[2].string, seconds)) {
                return make_error("ERR value is not an integer or out of range");
            }

            const std::string& key = request.elements[1].string;
            expire_if_due(store, key);

            if (store.values.count(key) == 0) {
                return make_integer(0);  // nothing to expire
            }

            // Redis deletes the key outright when the deadline is already past,
            // so that is what gets recorded too.
            if (seconds <= 0) {
                remove_key(store, key);
                log_del(log, key);
                return make_integer(1);
            }

            store.expirations[key] = Clock::now() + std::chrono::seconds(seconds);
            log_expire(log, key, seconds);
            return make_integer(1);
        }

        if (name == "TTL") {
            if (argc != 2) {
                return wrong_arity("ttl");
            }
            const std::string& key = request.elements[1].string;
            expire_if_due(store, key);

            if (store.values.count(key) == 0) {
                return make_integer(-2);  // missing, or already expired
            }

            const auto deadline = store.expirations.find(key);
            if (deadline == store.expirations.end()) {
                return make_integer(-1);  // exists, but lives forever
            }

            // Round up, so TTL immediately after EXPIRE key 10 reads 10, not 9.
            const auto remaining = deadline->second - Clock::now();
            return make_integer(std::chrono::ceil<std::chrono::seconds>(remaining).count());
        }

        return make_error("ERR unknown command '" + request.elements[0].string + "'");
    }

}  // namespace redis_lite
