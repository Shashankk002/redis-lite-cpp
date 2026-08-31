#include <iostream>
#include <string>
#include <vector>

#include "resp.hpp"

using redis_lite::ParseResult;
using redis_lite::ParseStatus;
using redis_lite::parse;
using redis_lite::RespType;
using redis_lite::RespValue;
using redis_lite::serialize;

static int failures = 0;

static std::string escape(const std::string& text) {
    std::string out;
    for (const char c : text) {
        if (c == '\r') {
            out += "\\r";
        } else if (c == '\n') {
            out += "\\n";
        } else {
            out += c;
        }
    }
    return out;
}

static void check(bool condition, const std::string& description) {
    if (condition) {
        std::cout << "  PASS  " << description << "\n";
    } else {
        std::cout << "  FAIL  " << description << "\n";
        ++failures;
    }
}

static void check_equal(const std::string& actual,
                        const std::string& expected,
                        const std::string& description) {
    if (actual == expected) {
        std::cout << "  PASS  " << description << "\n";
    } else {
        std::cout << "  FAIL  " << description << "\n";
        std::cout << "          expected: \"" << escape(expected) << "\"\n";
        std::cout << "          actual:   \"" << escape(actual) << "\"\n";
        ++failures;
    }
}

static std::string status_name(ParseStatus status) {
    switch (status) {
        case ParseStatus::Ok:         return "Ok";
        case ParseStatus::Incomplete: return "Incomplete";
        case ParseStatus::Malformed:  return "Malformed";
    }
    return "?";
}

static void check_status(const std::string& input,
                         ParseStatus expected,
                         const std::string& description) {
    const ParseResult result = parse(input);
    check_equal(status_name(result.status), status_name(expected), description);
}

// Parses `bytes`, serializes the result, and requires the bytes to match.
static void check_round_trip(const std::string& bytes, const std::string& description) {
    const ParseResult result = parse(bytes);
    if (result.status != ParseStatus::Ok) {
        check(false, description + " (did not parse: " + status_name(result.status) + ")");
        return;
    }
    check_equal(serialize(result.value), bytes, description);
}

static void test_stage_0_sanity() {
    std::cout << "\n-- stage 0 sanity --\n";
    check(1 + 1 == 2, "arithmetic works");
    check(std::string("redis").size() == 5, "std::string reports its length");
}

static void test_simple_strings() {
    std::cout << "\n-- simple strings --\n";

    const ParseResult ok = parse("+OK\r\n");
    check(ok.status == ParseStatus::Ok, "+OK\\r\\n parses");
    check(ok.value.type == RespType::SimpleString, "+OK\\r\\n is a simple string");
    check_equal(ok.value.string, "OK", "+OK\\r\\n carries \"OK\"");
    check(ok.consumed == 5, "+OK\\r\\n consumes 5 bytes");

    const ParseResult empty = parse("+\r\n");
    check(empty.status == ParseStatus::Ok, "an empty simple string parses");
    check_equal(empty.value.string, "", "an empty simple string carries \"\"");
}

static void test_errors() {
    std::cout << "\n-- errors --\n";

    const ParseResult result = parse("-ERR unknown command 'foo'\r\n");
    check(result.status == ParseStatus::Ok, "an error parses");
    check(result.value.type == RespType::Error, "it is tagged as an error");
    check_equal(result.value.string, "ERR unknown command 'foo'", "the message survives");
}

static void test_integers() {
    std::cout << "\n-- integers --\n";

    const ParseResult positive = parse(":100\r\n");
    check(positive.status == ParseStatus::Ok, ":100\\r\\n parses");
    check(positive.value.type == RespType::Integer, ":100\\r\\n is an integer");
    check(positive.value.integer == 100, ":100\\r\\n is 100");

    check(parse(":-5\r\n").value.integer == -5, ":-5\\r\\n is -5");
    check(parse(":0\r\n").value.integer == 0, ":0\\r\\n is 0");
}

static void test_bulk_strings() {
    std::cout << "\n-- bulk strings --\n";

    const ParseResult hello = parse("$5\r\nhello\r\n");
    check(hello.status == ParseStatus::Ok, "$5\\r\\nhello\\r\\n parses");
    check(hello.value.type == RespType::BulkString, "it is a bulk string");
    check_equal(hello.value.string, "hello", "it carries \"hello\"");
    check(hello.consumed == 11, "it consumes 11 bytes");
    check(!hello.value.is_null, "it is not null");

    const ParseResult empty = parse("$0\r\n\r\n");
    check(empty.status == ParseStatus::Ok, "an empty bulk string parses");
    check_equal(empty.value.string, "", "an empty bulk string carries \"\"");
    check(!empty.value.is_null, "an empty bulk string is not null");

    const ParseResult null_bulk = parse("$-1\r\n");
    check(null_bulk.status == ParseStatus::Ok, "a null bulk string parses");
    check(null_bulk.value.is_null, "a null bulk string is null");

    // Read by length, so the payload may itself contain CRLF.
    const ParseResult binary = parse("$4\r\na\r\nb\r\n");
    check(binary.status == ParseStatus::Ok, "a bulk string containing CRLF parses");
    check_equal(binary.value.string, "a\r\nb", "its embedded CRLF is preserved");
}

static void test_arrays() {
    std::cout << "\n-- arrays --\n";

    // The shape every Redis command arrives in.
    const ParseResult command = parse("*2\r\n$3\r\nGET\r\n$4\r\nname\r\n");
    check(command.status == ParseStatus::Ok, "*2 GET name parses");
    check(command.value.type == RespType::Array, "it is an array");
    check(command.value.elements.size() == 2, "it has 2 elements");
    check_equal(command.value.elements[0].string, "GET", "element 0 is \"GET\"");
    check_equal(command.value.elements[1].string, "name", "element 1 is \"name\"");
    check(command.value.elements[0].type == RespType::BulkString,
          "element 0 is a bulk string");

    const ParseResult empty = parse("*0\r\n");
    check(empty.status == ParseStatus::Ok, "an empty array parses");
    check(empty.value.elements.empty(), "an empty array has no elements");

    const ParseResult null_array = parse("*-1\r\n");
    check(null_array.status == ParseStatus::Ok, "a null array parses");
    check(null_array.value.is_null, "a null array is null");

    // Nesting falls out of the recursion for free.
    const ParseResult nested = parse("*2\r\n*2\r\n:1\r\n:2\r\n+OK\r\n");
    check(nested.status == ParseStatus::Ok, "a nested array parses");
    check(nested.value.elements.size() == 2, "the outer array has 2 elements");
    check(nested.value.elements[0].type == RespType::Array, "element 0 is itself an array");
    check(nested.value.elements[0].elements.size() == 2, "the inner array has 2 elements");
    check(nested.value.elements[0].elements[1].integer == 2, "the inner array holds 1, 2");
    check_equal(nested.value.elements[1].string, "OK", "element 1 is \"OK\"");
}

static void test_incomplete_input() {
    std::cout << "\n-- incomplete input --\n";

    check_status("", ParseStatus::Incomplete, "no bytes at all is incomplete");
    check_status("+OK", ParseStatus::Incomplete, "a simple string without CRLF is incomplete");
    check_status("+OK\r", ParseStatus::Incomplete, "a half-written CRLF is incomplete");
    check_status("$5\r\nhel", ParseStatus::Incomplete, "a short bulk payload is incomplete");
    check_status("$5\r\nhello", ParseStatus::Incomplete,
                 "a bulk payload without its CRLF is incomplete");
    check_status("*2\r\n$3\r\nGET\r\n", ParseStatus::Incomplete,
                 "an array missing its second element is incomplete");

    // The scenario that matters: one request split across two TCP reads.
    std::string buffer = "*2\r\n$3\r\nGET\r\n";
    check(parse(buffer).status == ParseStatus::Incomplete, "read 1 of 2 is incomplete");

    buffer += "$4\r\nname\r\n";
    const ParseResult joined = parse(buffer);
    check(joined.status == ParseStatus::Ok, "read 2 of 2 completes the request");
    check(joined.value.elements.size() == 2, "the rejoined request has 2 elements");
    check_equal(joined.value.elements[1].string, "name", "the rejoined request ends with \"name\"");
}

static void test_malformed_input() {
    std::cout << "\n-- malformed input --\n";

    check_status("!oops\r\n", ParseStatus::Malformed, "an unknown type byte is malformed");
    check_status(":abc\r\n", ParseStatus::Malformed, "a non-numeric integer is malformed");
    check_status(":\r\n", ParseStatus::Malformed, "an empty integer is malformed");
    check_status(":12x\r\n", ParseStatus::Malformed, "trailing junk in an integer is malformed");
    check_status("$abc\r\n", ParseStatus::Malformed, "a non-numeric bulk length is malformed");
    check_status("$-2\r\n", ParseStatus::Malformed, "bulk length below -1 is malformed");
    check_status("*-2\r\n", ParseStatus::Malformed, "array length below -1 is malformed");
    check_status("$5\r\nhelloXX", ParseStatus::Malformed,
                 "a bulk string not terminated by CRLF is malformed");
    check_status("*1\r\n!bad\r\n", ParseStatus::Malformed,
                 "a bad element makes the whole array malformed");

    const ParseResult bad = parse("!oops\r\n");
    check(!bad.error.empty(), "a malformed parse explains itself");
}

static void test_crlf_handling() {
    std::cout << "\n-- CRLF handling --\n";

    check_status("+OK\nBAD\r\n", ParseStatus::Malformed, "a bare LF in a line is malformed");
    check_status("+OK\rBAD\r\n", ParseStatus::Malformed, "a bare CR in a line is malformed");
    check_status("+OK\r\n", ParseStatus::Ok, "a proper CRLF is accepted");

    // Two values back to back: only the first is consumed.
    const ParseResult first = parse("+OK\r\n+SECOND\r\n");
    check(first.status == ParseStatus::Ok, "the first of two values parses");
    check(first.consumed == 5, "only the first value's bytes are consumed");

    const std::string rest = std::string("+OK\r\n+SECOND\r\n").substr(first.consumed);
    check_equal(parse(rest).value.string, "SECOND", "the leftover bytes are the next value");
}

static void test_serialization() {
    std::cout << "\n-- serialization --\n";

    check_equal(serialize(redis_lite::make_simple_string("OK")), "+OK\r\n",
                "a simple string serializes");
    check_equal(serialize(redis_lite::make_error("ERR boom")), "-ERR boom\r\n",
                "an error serializes");
    check_equal(serialize(redis_lite::make_integer(-42)), ":-42\r\n",
                "an integer serializes");
    check_equal(serialize(redis_lite::make_bulk_string("hello")), "$5\r\nhello\r\n",
                "a bulk string serializes");
    check_equal(serialize(redis_lite::make_bulk_string("")), "$0\r\n\r\n",
                "an empty bulk string serializes");
    check_equal(serialize(redis_lite::make_null_bulk_string()), "$-1\r\n",
                "a null bulk string serializes");

    const RespValue command = redis_lite::make_array({
        redis_lite::make_bulk_string("GET"),
        redis_lite::make_bulk_string("name"),
    });
    check_equal(serialize(command), "*2\r\n$3\r\nGET\r\n$4\r\nname\r\n",
                "an array serializes");
}

static void test_round_trips() {
    std::cout << "\n-- round trips: bytes -> value -> bytes --\n";

    check_round_trip("+OK\r\n", "simple string");
    check_round_trip("-ERR unknown command\r\n", "error");
    check_round_trip(":100\r\n", "integer");
    check_round_trip(":-5\r\n", "negative integer");
    check_round_trip("$5\r\nhello\r\n", "bulk string");
    check_round_trip("$0\r\n\r\n", "empty bulk string");
    check_round_trip("$-1\r\n", "null bulk string");
    check_round_trip("$4\r\na\r\nb\r\n", "bulk string containing CRLF");
    check_round_trip("*0\r\n", "empty array");
    check_round_trip("*-1\r\n", "null array");
    check_round_trip("*2\r\n$3\r\nGET\r\n$4\r\nname\r\n", "GET name command");
    check_round_trip("*2\r\n*2\r\n:1\r\n:2\r\n+OK\r\n", "nested array");
}

int main() {
    std::cout << "running tests\n";

    test_stage_0_sanity();
    test_simple_strings();
    test_errors();
    test_integers();
    test_bulk_strings();
    test_arrays();
    test_incomplete_input();
    test_malformed_input();
    test_crlf_handling();
    test_serialization();
    test_round_trips();

    std::cout << "\n";
    if (failures == 0) {
        std::cout << "all tests passed\n";
        return 0;
    }

    std::cout << failures << " check(s) failed\n";
    return 1;
}
