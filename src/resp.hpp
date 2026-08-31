#ifndef REDIS_LITE_RESP_HPP
#define REDIS_LITE_RESP_HPP

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace redis_lite {

    // The RESP2 types that Redis commands and replies are built from.
    enum class RespType {
        SimpleString,  // +OK\r\n
        Error,         // -ERR unknown command\r\n
        Integer,       // :100\r\n
        BulkString,    // $5\r\nhello\r\n
        Array,         // *2\r\n$3\r\nGET\r\n$4\r\nname\r\n
    };

    struct RespValue {
        RespType type = RespType::SimpleString;

        std::string string; //string
        long long integer = 0; //integer
        std::vector<RespValue> elements; //array

        bool is_null = false;
    };

    enum class ParseStatus {
        Ok,          // a complete value was parsed
        Incomplete,  // valid so far, but the value needs more bytes
        Malformed,   // not valid RESP, and more bytes will not help
    };

    struct ParseResult {
        ParseStatus status = ParseStatus::Incomplete;

        
        RespValue value; // Meaningful only when status == Ok.

        
        std::size_t consumed = 0; // How many bytes of the input that value used.
        // from the front of its buffer; whatever remains is the next request.

        std::string error;
    };

    // Parses the first RESP value in `input`.
    ParseResult parse(std::string_view input);

    // Converts a value back into RESP bytes.
    std::string serialize(const RespValue& value);

    // Small constructors, so building a value is one readable line.
    RespValue make_simple_string(std::string text);
    RespValue make_error(std::string text);
    RespValue make_integer(long long number);
    RespValue make_bulk_string(std::string text);
    RespValue make_null_bulk_string();
    RespValue make_array(std::vector<RespValue> elements);

}  // namespace redis_lite

#endif  // REDIS_LITE_RESP_HPP
