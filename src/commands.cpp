#include "commands.hpp"

#include <cctype>

namespace redis_lite {

    namespace {

        //We use to_upper to make Command names are case-insensitive: ping, PING and PiNg are one command.
        std::string to_upper(const std::string& text) {
            std::string out = text;
            for (char& c : out) {
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }
            return out;
        }

        RespValue wrong_arity(const std::string& name) {
            return make_error("ERR wrong number of arguments for '" + name + "' command");
        }

    }  // namespace

    RespValue execute_command(const RespValue& request, Store& store) {
        // Clients send commands as an array of bulk strings; anything else is a client bug, not a command.
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
            store[request.elements[1].string] = request.elements[2].string;
            return make_simple_string("OK");
        }

        if (name == "GET") {
            if (argc != 2) {
                return wrong_arity("get");
            }
            const auto found = store.find(request.elements[1].string);
            if (found == store.end()) {
                return make_null_bulk_string();  // $-1: the key does not exist
            }
            return make_bulk_string(found->second);
        }

        if (name == "DEL") {
            if (argc != 2) {
                return wrong_arity("del");
            }
            const bool erased = store.erase(request.elements[1].string) > 0;
            return make_integer(erased ? 1 : 0);
        }

        return make_error("ERR unknown command '" + request.elements[0].string + "'");
    }

}  // namespace redis_lite
