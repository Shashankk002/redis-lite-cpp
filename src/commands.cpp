#include "commands.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>

#include "persistence.hpp"

namespace redis_lite {

    namespace {

        using Clock = std::chrono::steady_clock;

        std::string to_upper(const std::string& text) {
            std::string out = text;
            for (char& c : out) {
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }
            return out;
        }

        bool to_integer(const std::string& text, long long& out) {
            const std::from_chars_result result =
                std::from_chars(text.data(), text.data() + text.size(), out);

            return result.ec == std::errc() && result.ptr == text.data() + text.size();
        }

        RespValue wrong_arity(const std::string& name) {
            return make_error("ERR wrong number of arguments for '" + name + "' command");
        }

        RespValue wrong_type() {
            return make_error("WRONGTYPE Operation against a key holding the wrong kind of value");
        }

        RespValue not_an_integer() {
            return make_error("ERR value is not an integer or out of range");
        }

        void remove_key(Store& store, const std::string& key) {
            store.values.erase(key);
            store.expirations.erase(key);
        }

        // Lazy expiration: drop the key if its deadline has passed.
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

        template <typename T>
        T* read_typed(Store& store, const std::string& key, bool& mismatch) {
            mismatch = false;
            expire_if_due(store, key);

            const auto found = store.values.find(key);
            if (found == store.values.end()) {
                return nullptr;
            }

            T* typed = std::get_if<T>(&found->second);
            if (typed == nullptr) {
                mismatch = true;
            }
            return typed;
        }

        template <typename T>
        T* write_typed(Store& store, const std::string& key) {
            expire_if_due(store, key);

            auto found = store.values.find(key);
            if (found == store.values.end()) {
                found = store.values.emplace(key, T{}).first;
            }
            return std::get_if<T>(&found->second);
        }


        void drop_if_empty(Store& store, const std::string& key) {
            const auto found = store.values.find(key);
            if (found == store.values.end()) {
                return;
            }

            bool empty = false;
            if (const ListValue* list = std::get_if<ListValue>(&found->second)) {
                empty = list->empty();
            } else if (const HashValue* hash = std::get_if<HashValue>(&found->second)) {
                empty = hash->empty();
            } else if (const SetValue* set = std::get_if<SetValue>(&found->second)) {
                empty = set->empty();
            }

            if (empty) {
                remove_key(store, key);
            }
        }

        
        long long absolute_index(long long index, long long size) {
            if (index < 0) {
                return index + size;
            }
            return index;
        }

    }  // namespace

    RespValue execute_command(const RespValue& request, Store& store, std::ofstream* log) {
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

        if (name == "DEL") {
            if (argc != 2) {
                return wrong_arity("del");
            }
            const std::string& key = request.elements[1].string;
            expire_if_due(store, key);

            const bool existed = store.values.count(key) > 0;
            remove_key(store, key);

            if (existed) {
                log_command(log, "DEL", {key});
            }
            return make_integer(existed ? 1 : 0);
        }

        if (name == "EXPIRE") {
            if (argc != 3) {
                return wrong_arity("expire");
            }

            long long seconds = 0;
            if (!to_integer(request.elements[2].string, seconds)) {
                return not_an_integer();
            }

            const std::string& key = request.elements[1].string;
            expire_if_due(store, key);

            if (store.values.count(key) == 0) {
                return make_integer(0);  // nothing to expire
            }

            if (seconds <= 0) {
                remove_key(store, key);
                log_command(log, "DEL", {key});
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

        // Strings
        if (name == "SET") {
            if (argc != 3) {
                return wrong_arity("set");
            }
            const std::string& key = request.elements[1].string;

            // SET replaces whatever was there, of any type, and clears the TTL.
            store.values[key] = StringValue{request.elements[2].string};
            store.expirations.erase(key);

            log_command(log, "SET", {key, request.elements[2].string});
            return make_simple_string("OK");
        }

        if (name == "GET") {
            if (argc != 2) {
                return wrong_arity("get");
            }
            const std::string& key = request.elements[1].string;

            bool mismatch = false;
            const StringValue* value = read_typed<StringValue>(store, key, mismatch);
            if (mismatch) {
                return wrong_type();
            }
            if (value == nullptr) {
                return make_null_bulk_string();  // $-1: the key does not exist
            }
            return make_bulk_string(*value);
        }

        // Lists
        if (name == "LPUSH" || name == "RPUSH") {
            if (argc != 3) {
                return wrong_arity(name == "LPUSH" ? "lpush" : "rpush");
            }
            const std::string& key = request.elements[1].string;

            ListValue* list = write_typed<ListValue>(store, key);
            if (list == nullptr) {
                return wrong_type();
            }

            if (name == "LPUSH") {
                list->push_front(request.elements[2].string);
            } else {
                list->push_back(request.elements[2].string);
            }

            log_command(log, name, {key, request.elements[2].string});
            return make_integer(static_cast<long long>(list->size()));
        }

        if (name == "LPOP" || name == "RPOP") {
            if (argc != 2) {
                return wrong_arity(name == "LPOP" ? "lpop" : "rpop");
            }
            const std::string& key = request.elements[1].string;

            bool mismatch = false;
            ListValue* list = read_typed<ListValue>(store, key, mismatch);
            if (mismatch) {
                return wrong_type();
            }
            if (list == nullptr || list->empty()) {
                return make_null_bulk_string();
            }

            std::string popped;
            if (name == "LPOP") {
                popped = list->front();
                list->pop_front();
            } else {
                popped = list->back();
                list->pop_back();
            }

            drop_if_empty(store, key);
            log_command(log, name, {key});
            return make_bulk_string(popped);
        }

        if (name == "LLEN") {
            if (argc != 2) {
                return wrong_arity("llen");
            }

            bool mismatch = false;
            const ListValue* list =
                read_typed<ListValue>(store, request.elements[1].string, mismatch);
            if (mismatch) {
                return wrong_type();
            }
            if (list == nullptr) {
                return make_integer(0);  // a missing key is an empty list
            }
            return make_integer(static_cast<long long>(list->size()));
        }

        if (name == "LRANGE") {
            if (argc != 4) {
                return wrong_arity("lrange");
            }

            long long start = 0;
            long long stop = 0;
            if (!to_integer(request.elements[2].string, start) ||
                !to_integer(request.elements[3].string, stop)) {
                return not_an_integer();
            }

            bool mismatch = false;
            const ListValue* list =
                read_typed<ListValue>(store, request.elements[1].string, mismatch);
            if (mismatch) {
                return wrong_type();
            }
            if (list == nullptr) {
                return make_array({});  // a missing key is an empty list
            }

            const long long size = static_cast<long long>(list->size());
            start = std::max<long long>(0, absolute_index(start, size));
            stop = std::min<long long>(size - 1, absolute_index(stop, size));

            std::vector<RespValue> elements;
            for (long long i = start; i <= stop; ++i) {
                elements.push_back(make_bulk_string((*list)[static_cast<std::size_t>(i)]));
            }
            return make_array(elements);
        }

        // Hashes
        if (name == "HSET") {
            if (argc != 4) {
                return wrong_arity("hset");
            }
            const std::string& key = request.elements[1].string;
            const std::string& field = request.elements[2].string;

            HashValue* hash = write_typed<HashValue>(store, key);
            if (hash == nullptr) {
                return wrong_type();
            }

            // 1 when the field is new, 0 when an existing field was updated.
            const bool is_new = hash->count(field) == 0;
            (*hash)[field] = request.elements[3].string;

            log_command(log, "HSET", {key, field, request.elements[3].string});
            return make_integer(is_new ? 1 : 0);
        }

        if (name == "HGET") {
            if (argc != 3) {
                return wrong_arity("hget");
            }

            bool mismatch = false;
            const HashValue* hash =
                read_typed<HashValue>(store, request.elements[1].string, mismatch);
            if (mismatch) {
                return wrong_type();
            }
            if (hash == nullptr) {
                return make_null_bulk_string();
            }

            const auto field = hash->find(request.elements[2].string);
            if (field == hash->end()) {
                return make_null_bulk_string();
            }
            return make_bulk_string(field->second);
        }

        if (name == "HDEL") {
            if (argc != 3) {
                return wrong_arity("hdel");
            }
            const std::string& key = request.elements[1].string;

            bool mismatch = false;
            HashValue* hash = read_typed<HashValue>(store, key, mismatch);
            if (mismatch) {
                return wrong_type();
            }
            if (hash == nullptr) {
                return make_integer(0);
            }

            const bool removed = hash->erase(request.elements[2].string) > 0;
            if (removed) {
                drop_if_empty(store, key);
                log_command(log, "HDEL", {key, request.elements[2].string});
            }
            return make_integer(removed ? 1 : 0);
        }

        if (name == "HEXISTS") {
            if (argc != 3) {
                return wrong_arity("hexists");
            }

            bool mismatch = false;
            const HashValue* hash =
                read_typed<HashValue>(store, request.elements[1].string, mismatch);
            if (mismatch) {
                return wrong_type();
            }
            if (hash == nullptr) {
                return make_integer(0);
            }
            return make_integer(hash->count(request.elements[2].string) > 0 ? 1 : 0);
        }

        if (name == "HLEN") {
            if (argc != 2) {
                return wrong_arity("hlen");
            }

            bool mismatch = false;
            const HashValue* hash =
                read_typed<HashValue>(store, request.elements[1].string, mismatch);
            if (mismatch) {
                return wrong_type();
            }
            if (hash == nullptr) {
                return make_integer(0);
            }
            return make_integer(static_cast<long long>(hash->size()));
        }

        // Sets
        if (name == "SADD") {
            if (argc != 3) {
                return wrong_arity("sadd");
            }
            const std::string& key = request.elements[1].string;

            SetValue* set = write_typed<SetValue>(store, key);
            if (set == nullptr) {
                return wrong_type();
            }

            // 1 when the member was actually added, 0 when it was already there.
            const bool added = set->insert(request.elements[2].string).second;
            if (added) {
                log_command(log, "SADD", {key, request.elements[2].string});
            }
            return make_integer(added ? 1 : 0);
        }

        if (name == "SREM") {
            if (argc != 3) {
                return wrong_arity("srem");
            }
            const std::string& key = request.elements[1].string;

            bool mismatch = false;
            SetValue* set = read_typed<SetValue>(store, key, mismatch);
            if (mismatch) {
                return wrong_type();
            }
            if (set == nullptr) {
                return make_integer(0);
            }

            const bool removed = set->erase(request.elements[2].string) > 0;
            if (removed) {
                drop_if_empty(store, key);
                log_command(log, "SREM", {key, request.elements[2].string});
            }
            return make_integer(removed ? 1 : 0);
        }

        if (name == "SISMEMBER") {
            if (argc != 3) {
                return wrong_arity("sismember");
            }

            bool mismatch = false;
            const SetValue* set =
                read_typed<SetValue>(store, request.elements[1].string, mismatch);
            if (mismatch) {
                return wrong_type();
            }
            if (set == nullptr) {
                return make_integer(0);
            }
            return make_integer(set->count(request.elements[2].string) > 0 ? 1 : 0);
        }

        if (name == "SCARD") {
            if (argc != 2) {
                return wrong_arity("scard");
            }

            bool mismatch = false;
            const SetValue* set =
                read_typed<SetValue>(store, request.elements[1].string, mismatch);
            if (mismatch) {
                return wrong_type();
            }
            if (set == nullptr) {
                return make_integer(0);
            }
            return make_integer(static_cast<long long>(set->size()));
        }

        if (name == "SMEMBERS") {
            if (argc != 2) {
                return wrong_arity("smembers");
            }

            bool mismatch = false;
            const SetValue* set =
                read_typed<SetValue>(store, request.elements[1].string, mismatch);
            if (mismatch) {
                return wrong_type();
            }
            if (set == nullptr) {
                return make_array({});
            }

            std::vector<RespValue> elements;
            for (const std::string& member : *set) {
                elements.push_back(make_bulk_string(member));
            }
            return make_array(elements);
        }

        return make_error("ERR unknown command '" + request.elements[0].string + "'");
    }

}  // namespace redis_lite
