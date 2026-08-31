#include "resp.hpp"

#include <charconv>  // std::from_chars

namespace redis_lite {
    namespace {

        enum class LineStatus {
            Ok,
            Incomplete,
            Malformed,
        };

        LineStatus read_line(std::string_view input, std::size_t& pos, std::string_view& line) {
            const std::size_t crlf = input.find("\r\n", pos);
            if (crlf == std::string_view::npos) {
                return LineStatus::Incomplete;
            }

            line = input.substr(pos, crlf - pos);

            if (line.find('\r') != std::string_view::npos ||
                line.find('\n') != std::string_view::npos) {
                return LineStatus::Malformed;
            }

            pos = crlf + 2;
            return LineStatus::Ok;
        }

        bool to_integer(std::string_view text, long long& out) {
            const std::from_chars_result result =
                std::from_chars(text.data(), text.data() + text.size(), out);

            return result.ec == std::errc() && result.ptr == text.data() + text.size();
        }

        ParseStatus parse_value(std::string_view input,
                                std::size_t& pos,
                                RespValue& out,
                                std::string& error) {
            if (pos >= input.size()) {
                return ParseStatus::Incomplete;
            }

            const char prefix = input[pos];
            pos += 1;

            std::string_view line;
            if (prefix == '+' || prefix == '-' || prefix == ':' ||
                prefix == '$' || prefix == '*') {
                const LineStatus status = read_line(input, pos, line);
                if (status == LineStatus::Incomplete) {
                    return ParseStatus::Incomplete;
                }
                if (status == LineStatus::Malformed) {
                    error = "line contains a stray CR or LF";
                    return ParseStatus::Malformed;
                }
            }

            switch (prefix) {
                case '+':
                    out.type = RespType::SimpleString;
                    out.string = std::string(line);
                    return ParseStatus::Ok;

                case '-':
                    out.type = RespType::Error;
                    out.string = std::string(line);
                    return ParseStatus::Ok;

                case ':': {
                    long long number = 0;
                    if (!to_integer(line, number)) {
                        error = "integer is not a valid number";
                        return ParseStatus::Malformed;
                    }
                    out.type = RespType::Integer;
                    out.integer = number;
                    return ParseStatus::Ok;
                }

                case '$': {
                    long long length = 0;
                    if (!to_integer(line, length)) {
                        error = "bulk string length is not a valid number";
                        return ParseStatus::Malformed;
                    }

                    out.type = RespType::BulkString;

                    if (length == -1) {
                        out.is_null = true;
                        return ParseStatus::Ok;
                    }
                    if (length < -1) {
                        error = "negative bulk string length";
                        return ParseStatus::Malformed;
                    }

                    const std::size_t needed = static_cast<std::size_t>(length) + 2;
                    if (input.size() - pos < needed) {
                        return ParseStatus::Incomplete;
                    }

                    const std::size_t size = static_cast<std::size_t>(length);
                    if (input[pos + size] != '\r' || input[pos + size + 1] != '\n') {
                        error = "bulk string is not terminated by CRLF";
                        return ParseStatus::Malformed;
                    }

                    // Read by length, so the data may contain CR, LF or NUL bytes.
                    out.string = std::string(input.substr(pos, size));
                    pos += needed;
                    return ParseStatus::Ok;
                }

                case '*': {
                    long long count = 0;
                    if (!to_integer(line, count)) {
                        error = "array length is not a valid number";
                        return ParseStatus::Malformed;
                    }

                    out.type = RespType::Array;

                    if (count == -1) {
                        out.is_null = true;
                        return ParseStatus::Ok;
                    }
                    if (count < -1) {
                        error = "negative array length";
                        return ParseStatus::Malformed;
                    }

                    out.elements.reserve(static_cast<std::size_t>(count));
                    for (long long i = 0; i < count; ++i) {
                        RespValue element;
                        const ParseStatus status = parse_value(input, pos, element, error);
                        if (status != ParseStatus::Ok) {
                            return status;  // Incomplete or Malformed, unchanged
                        }
                        out.elements.push_back(std::move(element));
                    }
                    return ParseStatus::Ok;
                }

                default:
                    error = "unknown RESP type byte";
                    return ParseStatus::Malformed;
            }
        }

    }  // namespace

    ParseResult parse(std::string_view input) {
        ParseResult result;
        std::size_t pos = 0;

        result.status = parse_value(input, pos, result.value, result.error);
        if (result.status == ParseStatus::Ok) {
            result.consumed = pos;
        }
        return result;
    }

    std::string serialize(const RespValue& value) {
        switch (value.type) {
            case RespType::SimpleString:
                return "+" + value.string + "\r\n";

            case RespType::Error:
                return "-" + value.string + "\r\n";

            case RespType::Integer:
                return ":" + std::to_string(value.integer) + "\r\n";

            case RespType::BulkString:
                if (value.is_null) {
                    return "$-1\r\n";
                }
                return "$" + std::to_string(value.string.size()) + "\r\n" +
                       value.string + "\r\n";

            case RespType::Array: {
                if (value.is_null) {
                    return "*-1\r\n";
                }
                std::string out = "*" + std::to_string(value.elements.size()) + "\r\n";
                for (const RespValue& element : value.elements) {
                    out += serialize(element);
                }
                return out;
            }
        }
        return {};  // unreachable; keeps the compiler happy
    }

    RespValue make_simple_string(std::string text) {
        RespValue value;
        value.type = RespType::SimpleString;
        value.string = std::move(text);
        return value;
    }

    RespValue make_error(std::string text) {
        RespValue value;
        value.type = RespType::Error;
        value.string = std::move(text);
        return value;
    }

    RespValue make_integer(long long number) {
        RespValue value;
        value.type = RespType::Integer;
        value.integer = number;
        return value;
    }

    RespValue make_bulk_string(std::string text) {
        RespValue value;
        value.type = RespType::BulkString;
        value.string = std::move(text);
        return value;
    }

    RespValue make_null_bulk_string() {
        RespValue value;
        value.type = RespType::BulkString;
        value.is_null = true;
        return value;
    }

    RespValue make_array(std::vector<RespValue> elements) {
        RespValue value;
        value.type = RespType::Array;
        value.elements = std::move(elements);
        return value;
    }

}  // namespace redis_lite
