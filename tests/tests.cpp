#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <variant>
#include <utility>
#include <vector>

#include "commands.hpp"
#include "persistence.hpp"
#include "resp.hpp"

using redis_lite::ParseResult;
using redis_lite::ParseStatus;
using redis_lite::parse;
using redis_lite::RespType;
using redis_lite::RespValue;
using redis_lite::serialize;
using redis_lite::Store;
using redis_lite::execute_command;

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

    // A count far larger than the input must be reported the same way a short
    // bulk string is, without reserving room for the elements it claims.
    check_status("*1000000000\r\n", ParseStatus::Incomplete,
                 "an array count larger than the input is incomplete");
    check_status("*9223372036854775807\r\n", ParseStatus::Incomplete,
                 "an array count at the top of long long is incomplete");
    check_status("*1000000000\r\n:1\r\n:2\r\n", ParseStatus::Incomplete,
                 "a huge count with a few real elements is still incomplete");

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

// Builds a request array from plain strings and executes it, the way a client
// would: run(store, {"SET", "name", "Shashank"}).
static std::string run(Store& store, const std::vector<std::string>& args) {
    std::vector<RespValue> elements;
    for (const std::string& arg : args) {
        elements.push_back(redis_lite::make_bulk_string(arg));
    }
    return serialize(execute_command(redis_lite::make_array(elements), store));
}

// The full path: RESP bytes -> parse -> execute -> serialize -> RESP bytes.
static std::string execute_bytes(Store& store, const std::string& request) {
    const ParseResult parsed = parse(request);
    if (parsed.status != ParseStatus::Ok) {
        return "<parse failed: " + status_name(parsed.status) + ">";
    }
    return serialize(execute_command(parsed.value, store));
}

static void test_ping() {
    std::cout << "\n-- PING --\n";

    Store store;
    check_equal(run(store, {"PING"}), "+PONG\r\n", "PING replies +PONG");
}

static void test_set_and_get() {
    std::cout << "\n-- SET and GET --\n";

    Store store;
    check_equal(run(store, {"SET", "name", "Shashank"}), "+OK\r\n", "SET replies +OK");
    check_equal(run(store, {"GET", "name"}), "$8\r\nShashank\r\n", "GET returns the value");
    check_equal(run(store, {"GET", "missing"}), "$-1\r\n", "GET on a missing key is null");

    // Several keys coexist.
    check_equal(run(store, {"SET", "city", "Hyderabad"}), "+OK\r\n", "a second key can be set");
    check_equal(run(store, {"GET", "city"}), "$9\r\nHyderabad\r\n", "the second key reads back");
    check_equal(run(store, {"GET", "name"}), "$8\r\nShashank\r\n", "the first key is untouched");

    // Overwriting replaces the value.
    check_equal(run(store, {"SET", "name", "Kanneboina"}), "+OK\r\n", "an existing key can be set again");
    check_equal(run(store, {"GET", "name"}), "$10\r\nKanneboina\r\n", "the new value replaces the old");

    // Values are opaque bytes, not text.
    check_equal(run(store, {"SET", "empty", ""}), "+OK\r\n", "an empty value can be stored");
    check_equal(run(store, {"GET", "empty"}), "$0\r\n\r\n", "an empty value is not the same as missing");
}

static void test_del() {
    std::cout << "\n-- DEL --\n";

    Store store;
    run(store, {"SET", "name", "Shashank"});

    check_equal(run(store, {"DEL", "name"}), ":1\r\n", "DEL on an existing key returns 1");
    check_equal(run(store, {"GET", "name"}), "$-1\r\n", "the key is gone afterwards");
    check_equal(run(store, {"DEL", "name"}), ":0\r\n", "DEL on a missing key returns 0");
    check_equal(run(store, {"DEL", "never-existed"}), ":0\r\n", "DEL on an unknown key returns 0");
}

static void test_case_insensitivity() {
    std::cout << "\n-- case-insensitive command names --\n";

    Store store;
    check_equal(run(store, {"ping"}), "+PONG\r\n", "lowercase ping works");
    check_equal(run(store, {"PiNg"}), "+PONG\r\n", "mixed-case PiNg works");
    check_equal(run(store, {"set", "k", "v"}), "+OK\r\n", "lowercase set works");
    check_equal(run(store, {"GeT", "k"}), "$1\r\nv\r\n", "mixed-case GeT works");
    check_equal(run(store, {"dEl", "k"}), ":1\r\n", "mixed-case dEl works");

    // Keys, unlike command names, are case-sensitive.
    run(store, {"SET", "Key", "upper"});
    check_equal(run(store, {"GET", "key"}), "$-1\r\n", "keys stay case-sensitive");
}

static void test_command_errors() {
    std::cout << "\n-- invalid commands --\n";

    Store store;

    const std::string unknown = run(store, {"FLUSHALL"});
    check(unknown.rfind("-ERR unknown command", 0) == 0, "an unknown command is an error");
    check(unknown.find("FLUSHALL") != std::string::npos, "the error names the command");

    check(run(store, {"GET"}).rfind("-ERR wrong number", 0) == 0, "GET with no key is an error");
    check(run(store, {"GET", "a", "b"}).rfind("-ERR wrong number", 0) == 0, "GET with two keys is an error");
    check(run(store, {"SET"}).rfind("-ERR wrong number", 0) == 0, "SET with no arguments is an error");
    check(run(store, {"SET", "a"}).rfind("-ERR wrong number", 0) == 0, "SET without a value is an error");
    check(run(store, {"SET", "a", "b", "c"}).rfind("-ERR wrong number", 0) == 0, "SET with extra arguments is an error");
    check(run(store, {"DEL"}).rfind("-ERR wrong number", 0) == 0, "DEL with no key is an error");
    check(run(store, {"PING", "extra"}).rfind("-ERR wrong number", 0) == 0, "PING with an argument is an error");

    // Requests that are not an array of bulk strings at all.
    check(serialize(execute_command(redis_lite::make_simple_string("PING"), store))
              .rfind("-ERR", 0) == 0,
          "a non-array request is an error");
    check(serialize(execute_command(redis_lite::make_array({}), store)).rfind("-ERR", 0) == 0,
          "an empty array is an error");
    check(serialize(execute_command(redis_lite::make_array({redis_lite::make_integer(1)}), store))
              .rfind("-ERR", 0) == 0,
          "an array of non-bulk-strings is an error");

    // A failed command must not disturb stored data.
    run(store, {"SET", "survivor", "yes"});
    run(store, {"NOPE"});
    check_equal(run(store, {"GET", "survivor"}), "$3\r\nyes\r\n", "a bad command leaves the store intact");
}

static void test_resp_to_command_integration() {
    std::cout << "\n-- RESP bytes -> command -> RESP bytes --\n";

    Store store;

    check_equal(execute_bytes(store, "*1\r\n$4\r\nPING\r\n"),
                "+PONG\r\n", "PING over RESP");
    check_equal(execute_bytes(store, "*3\r\n$3\r\nSET\r\n$4\r\nname\r\n$8\r\nShashank\r\n"),
                "+OK\r\n", "SET over RESP");
    check_equal(execute_bytes(store, "*2\r\n$3\r\nGET\r\n$4\r\nname\r\n"),
                "$8\r\nShashank\r\n", "GET over RESP");
    check_equal(execute_bytes(store, "*2\r\n$3\r\nDEL\r\n$4\r\nname\r\n"),
                ":1\r\n", "DEL over RESP");
    check_equal(execute_bytes(store, "*2\r\n$3\r\nGET\r\n$4\r\nname\r\n"),
                "$-1\r\n", "GET over RESP after DEL is null");
}

static void test_buffer_loop() {
    std::cout << "\n-- several commands in one buffer --\n";

    // Exactly what the server does with the bytes one recv() handed it.
    Store store;
    std::string buffer =
        "*1\r\n$4\r\nPING\r\n"
        "*3\r\n$3\r\nSET\r\n$4\r\nname\r\n$8\r\nShashank\r\n"
        "*2\r\n$3\r\nGET\r\n$4\r\nname\r\n";

    std::vector<std::string> replies;
    while (true) {
        const ParseResult result = parse(buffer);
        if (result.status != ParseStatus::Ok) {
            break;
        }
        replies.push_back(serialize(execute_command(result.value, store)));
        buffer.erase(0, result.consumed);
    }

    check(replies.size() == 3, "three commands in one buffer produce three replies");
    check_equal(replies[0], "+PONG\r\n", "reply 1 is +PONG");
    check_equal(replies[1], "+OK\r\n", "reply 2 is +OK");
    check_equal(replies[2], "$8\r\nShashank\r\n", "reply 3 is the stored value");
    check(buffer.empty(), "the buffer is fully consumed");

    // A trailing partial command stays in the buffer untouched.
    buffer = "*1\r\n$4\r\nPING\r\n*2\r\n$3\r\nGET\r\n";
    const ParseResult first = parse(buffer);
    check(first.status == ParseStatus::Ok, "the complete command parses");
    buffer.erase(0, first.consumed);
    check_equal(buffer, "*2\r\n$3\r\nGET\r\n", "the partial command is preserved");
    check(parse(buffer).status == ParseStatus::Incomplete, "the leftover is incomplete, not malformed");
}

// Expiration tests need real elapsed time. Durations are kept short but with
// comfortable margins, so the suite stays quick without becoming flaky.
static void sleep_ms(int milliseconds) {
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

static void test_expire_basics() {
    std::cout << "\n-- EXPIRE --\n";

    Store store;
    run(store, {"SET", "foo", "bar"});

    check_equal(run(store, {"EXPIRE", "foo", "50"}), ":1\r\n", "EXPIRE on an existing key returns 1");
    check_equal(run(store, {"EXPIRE", "nosuchkey", "50"}), ":0\r\n", "EXPIRE on a missing key returns 0");
    check_equal(run(store, {"GET", "foo"}), "$3\r\nbar\r\n", "the key is still readable before it expires");
    check_equal(run(store, {"TTL", "foo"}), ":50\r\n", "TTL reports the seconds just set");

    // Re-running EXPIRE replaces the old deadline.
    check_equal(run(store, {"EXPIRE", "foo", "80"}), ":1\r\n", "EXPIRE can be applied again");
    check_equal(run(store, {"TTL", "foo"}), ":80\r\n", "the newer deadline replaces the older one");
}

static void test_ttl_meanings() {
    std::cout << "\n-- TTL return values --\n";

    Store store;
    run(store, {"SET", "forever", "value"});

    check_equal(run(store, {"TTL", "forever"}), ":-1\r\n", "TTL is -1 for a key with no expiration");
    check_equal(run(store, {"TTL", "nosuchkey"}), ":-2\r\n", "TTL is -2 for a missing key");

    run(store, {"EXPIRE", "forever", "30"});
    check_equal(run(store, {"TTL", "forever"}), ":30\r\n", "TTL is the remaining seconds once set");
}

static void test_key_actually_expires() {
    std::cout << "\n-- a key expires after its deadline --\n";

    Store store;
    run(store, {"SET", "foo", "bar"});
    run(store, {"EXPIRE", "foo", "1"});

    check_equal(run(store, {"GET", "foo"}), "$3\r\nbar\r\n", "GET before expiry returns the value");

    sleep_ms(1200);

    check_equal(run(store, {"GET", "foo"}), "$-1\r\n", "GET after expiry is null");
    check_equal(run(store, {"TTL", "foo"}), ":-2\r\n", "TTL after expiry is -2");
    check_equal(run(store, {"DEL", "foo"}), ":0\r\n", "DEL on an expired key returns 0");
    check_equal(run(store, {"EXPIRE", "foo", "50"}), ":0\r\n", "EXPIRE on an expired key returns 0");

    // The lazy sweep really removed it, rather than just hiding it.
    check(store.values.count("foo") == 0, "the expired value is gone from the store");
    check(store.expirations.count("foo") == 0, "the expired deadline is gone from the store");
}

static void test_ttl_decreases() {
    std::cout << "\n-- TTL counts down --\n";

    Store store;
    run(store, {"SET", "foo", "bar"});
    run(store, {"EXPIRE", "foo", "3"});

    check_equal(run(store, {"TTL", "foo"}), ":3\r\n", "TTL starts at 3");

    sleep_ms(1200);

    check_equal(run(store, {"TTL", "foo"}), ":2\r\n", "TTL is 2 a second later");
    check_equal(run(store, {"GET", "foo"}), "$3\r\nbar\r\n", "the key is still alive");
}

static void test_set_clears_ttl() {
    std::cout << "\n-- SET clears an existing TTL --\n";

    Store store;
    run(store, {"SET", "foo", "bar"});
    run(store, {"EXPIRE", "foo", "10"});
    check_equal(run(store, {"TTL", "foo"}), ":10\r\n", "the TTL is set");

    check_equal(run(store, {"SET", "foo", "new"}), "+OK\r\n", "the key is set again");
    check_equal(run(store, {"TTL", "foo"}), ":-1\r\n", "SET removed the old TTL");
    check_equal(run(store, {"GET", "foo"}), "$3\r\nnew\r\n", "the new value is in place");
}

static void test_del_clears_ttl() {
    std::cout << "\n-- DEL clears expiration information --\n";

    Store store;
    run(store, {"SET", "foo", "bar"});
    run(store, {"EXPIRE", "foo", "10"});

    check_equal(run(store, {"DEL", "foo"}), ":1\r\n", "DEL removes the key");
    check(store.expirations.count("foo") == 0, "DEL removed the deadline too");

    // A stale deadline would wrongly expire this brand new key.
    check_equal(run(store, {"SET", "foo", "fresh"}), "+OK\r\n", "the key is recreated");
    check_equal(run(store, {"TTL", "foo"}), ":-1\r\n", "the recreated key has no TTL");
}

static void test_expire_edge_values() {
    std::cout << "\n-- zero and negative expirations --\n";

    Store store;

    run(store, {"SET", "zero", "value"});
    check_equal(run(store, {"EXPIRE", "zero", "0"}), ":1\r\n", "EXPIRE 0 reports success");
    check_equal(run(store, {"GET", "zero"}), "$-1\r\n", "EXPIRE 0 removes the key immediately");
    check_equal(run(store, {"TTL", "zero"}), ":-2\r\n", "the key removed by EXPIRE 0 is gone");

    run(store, {"SET", "negative", "value"});
    check_equal(run(store, {"EXPIRE", "negative", "-1"}), ":1\r\n", "EXPIRE -1 reports success");
    check_equal(run(store, {"GET", "negative"}), "$-1\r\n", "EXPIRE -1 removes the key immediately");

    check_equal(run(store, {"EXPIRE", "nosuchkey", "0"}), ":0\r\n", "EXPIRE 0 on a missing key returns 0");
}

static void test_expire_and_ttl_errors() {
    std::cout << "\n-- invalid EXPIRE and TTL --\n";

    Store store;
    run(store, {"SET", "foo", "bar"});

    const std::string not_a_number = run(store, {"EXPIRE", "foo", "abc"});
    check(not_a_number.rfind("-ERR value is not an integer", 0) == 0,
          "EXPIRE with a non-numeric time is an error");
    check_equal(run(store, {"TTL", "foo"}), ":-1\r\n", "the failed EXPIRE set no TTL");

    check(run(store, {"EXPIRE", "foo", "1.5"}).rfind("-ERR value is not an integer", 0) == 0,
          "EXPIRE with a fractional time is an error");

    check(run(store, {"EXPIRE"}).rfind("-ERR wrong number", 0) == 0, "EXPIRE with no arguments is an error");
    check(run(store, {"EXPIRE", "foo"}).rfind("-ERR wrong number", 0) == 0, "EXPIRE without a time is an error");
    check(run(store, {"EXPIRE", "foo", "10", "extra"}).rfind("-ERR wrong number", 0) == 0,
          "EXPIRE with extra arguments is an error");

    check(run(store, {"TTL"}).rfind("-ERR wrong number", 0) == 0, "TTL with no key is an error");
    check(run(store, {"TTL", "foo", "extra"}).rfind("-ERR wrong number", 0) == 0,
          "TTL with extra arguments is an error");

    check_equal(run(store, {"GET", "foo"}), "$3\r\nbar\r\n", "invalid commands left the key alone");
}

static void test_independent_expirations() {
    std::cout << "\n-- keys expire independently --\n";

    Store store;
    run(store, {"SET", "quick", "a"});
    run(store, {"SET", "slow", "b"});
    run(store, {"SET", "forever", "c"});
    run(store, {"EXPIRE", "quick", "1"});
    run(store, {"EXPIRE", "slow", "60"});

    sleep_ms(1200);

    check_equal(run(store, {"GET", "quick"}), "$-1\r\n", "the short-lived key expired");
    check_equal(run(store, {"GET", "slow"}), "$1\r\nb\r\n", "the long-lived key survived");
    check_equal(run(store, {"GET", "forever"}), "$1\r\nc\r\n", "the key with no TTL survived");
    check_equal(run(store, {"TTL", "slow"}), ":59\r\n", "the survivor's TTL counted down");
    check_equal(run(store, {"TTL", "forever"}), ":-1\r\n", "the key with no TTL still reports -1");
}

static void test_ttl_over_resp() {
    std::cout << "\n-- EXPIRE and TTL over RESP --\n";

    Store store;

    check_equal(execute_bytes(store, "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n"),
                "+OK\r\n", "SET over RESP");
    check_equal(execute_bytes(store, "*3\r\n$6\r\nEXPIRE\r\n$3\r\nfoo\r\n$2\r\n30\r\n"),
                ":1\r\n", "EXPIRE over RESP");
    check_equal(execute_bytes(store, "*2\r\n$3\r\nTTL\r\n$3\r\nfoo\r\n"),
                ":30\r\n", "TTL over RESP");
    check_equal(execute_bytes(store, "*2\r\n$3\r\nttl\r\n$3\r\nfoo\r\n"),
                ":30\r\n", "lowercase ttl over RESP");
    check_equal(execute_bytes(store, "*3\r\n$6\r\nexpire\r\n$3\r\nfoo\r\n$1\r\n0\r\n"),
                ":1\r\n", "lowercase expire over RESP");
    check_equal(execute_bytes(store, "*2\r\n$3\r\nTTL\r\n$3\r\nfoo\r\n"),
                ":-2\r\n", "the key expired by EXPIRE 0 reports -2");
}

// Runs a command the way the threaded server does: the mutex is held for the
// store access only, and released before anything else happens.
static std::string run_locked(Store& store,
                              std::mutex& store_mutex,
                              const std::vector<std::string>& args) {
    std::vector<RespValue> elements;
    for (const std::string& arg : args) {
        elements.push_back(redis_lite::make_bulk_string(arg));
    }
    const RespValue request = redis_lite::make_array(elements);

    RespValue reply;
    {
        std::lock_guard<std::mutex> lock(store_mutex);
        reply = execute_command(request, store);
    }
    return serialize(reply);
}

static void test_concurrent_independent_keys() {
    std::cout << "\n-- concurrent clients, one key each --\n";

    Store store;
    std::mutex store_mutex;

    const int client_count = 8;
    const int writes_per_client = 250;

    std::vector<std::thread> clients;
    for (int id = 0; id < client_count; ++id) {
        clients.emplace_back([&store, &store_mutex, id]() {
            const std::string key = "client" + std::to_string(id);
            for (int i = 0; i < writes_per_client; ++i) {
                run_locked(store, store_mutex, {"SET", key, std::to_string(i)});
                run_locked(store, store_mutex, {"GET", key});
                run_locked(store, store_mutex, {"TTL", key});
            }
        });
    }
    for (std::thread& client : clients) {
        client.join();
    }

    check(store.values.size() == static_cast<std::size_t>(client_count),
          "every client's key survived, and no others appeared");

    bool all_correct = true;
    for (int id = 0; id < client_count; ++id) {
        const std::string expected = "$3\r\n249\r\n";
        if (run(store, {"GET", "client" + std::to_string(id)}) != expected) {
            all_correct = false;
        }
    }
    check(all_correct, "each key holds the last value its own client wrote");
}

static void test_concurrent_shared_key() {
    std::cout << "\n-- concurrent clients, one shared key --\n";

    Store store;
    std::mutex store_mutex;

    // Every client writes its own id to the same key, then reads it back. The
    // read may see another client's value -- that is normal, interleaved
    // access. What must never happen is a torn or invalid value.
    const int client_count = 8;
    std::vector<std::thread> clients;
    bool values_always_valid = true;
    std::mutex result_mutex;

    for (int id = 0; id < client_count; ++id) {
        clients.emplace_back([&, id]() {
            for (int i = 0; i < 300; ++i) {
                run_locked(store, store_mutex, {"SET", "shared", std::to_string(id)});

                const std::string reply = run_locked(store, store_mutex, {"GET", "shared"});
                // A single-digit bulk string reply is exactly "$1\r\nX\r\n" = 7 bytes.
                const bool looks_valid = reply.size() == 7 &&
                                         reply.rfind("$1\r\n", 0) == 0 &&
                                         reply[4] >= '0' && reply[4] <= '7';
                if (!looks_valid) {
                    std::lock_guard<std::mutex> lock(result_mutex);
                    values_always_valid = false;
                }
            }
        });
    }
    for (std::thread& client : clients) {
        client.join();
    }

    check(values_always_valid, "a shared key always reads back as a whole, valid value");
    check(store.values.count("shared") == 1, "the shared key exists exactly once");
}

static void test_concurrent_ttl() {
    std::cout << "\n-- TTL under concurrent access --\n";

    Store store;
    std::mutex store_mutex;

    // Half the clients keep setting expirations, half keep reading them, on
    // keys they share. Expirations and values are one logical state, so the
    // same mutex covers both.
    std::vector<std::thread> clients;
    for (int id = 0; id < 4; ++id) {
        clients.emplace_back([&store, &store_mutex, id]() {
            const std::string key = "ttlkey" + std::to_string(id % 2);
            for (int i = 0; i < 300; ++i) {
                run_locked(store, store_mutex, {"SET", key, "value"});
                run_locked(store, store_mutex, {"EXPIRE", key, "60"});
                run_locked(store, store_mutex, {"TTL", key});
                run_locked(store, store_mutex, {"DEL", key});
            }
        });
    }
    for (std::thread& client : clients) {
        client.join();
    }

    // Every DEL removes both halves, so nothing may be left behind.
    check(store.expirations.size() <= store.values.size(),
          "no expiration outlives the key it belongs to");

    // And expiration still behaves normally afterwards.
    run_locked(store, store_mutex, {"SET", "after", "value"});
    run_locked(store, store_mutex, {"EXPIRE", "after", "1"});
    check_equal(run_locked(store, store_mutex, {"TTL", "after"}), ":1\r\n",
                "TTL still works after the concurrent run");

    sleep_ms(1200);
    check_equal(run_locked(store, store_mutex, {"GET", "after"}), "$-1\r\n",
                "the key still expires after the concurrent run");
}

static void test_one_client_sets_another_gets() {
    std::cout << "\n-- one client SETs, another GETs --\n";

    Store store;
    std::mutex store_mutex;

    std::string writer_reply;
    std::thread writer([&]() {
        writer_reply = run_locked(store, store_mutex, {"SET", "foo", "bar"});
    });
    writer.join();

    std::string reader_reply;
    std::thread reader([&]() {
        reader_reply = run_locked(store, store_mutex, {"GET", "foo"});
    });
    reader.join();

    check_equal(writer_reply, "+OK\r\n", "the writing client got +OK");
    check_equal(reader_reply, "$3\r\nbar\r\n", "the reading client saw the other client's value");
}

// The five sections above date from Stage 6, when the server ran one thread per
// client. Stage 7 replaced that with a single-threaded event loop, so the server
// no longer takes a lock -- but these still document that the store behaves
// correctly under concurrent access, so they are kept.
static void test_concurrent_pings() {
    std::cout << "\n-- independent PINGs --\n";

    Store store;
    std::mutex store_mutex;

    std::vector<std::string> replies(4);
    std::vector<std::thread> clients;
    for (int id = 0; id < 4; ++id) {
        clients.emplace_back([&replies, &store, &store_mutex, id]() {
            replies[static_cast<std::size_t>(id)] = run_locked(store, store_mutex, {"PING"});
        });
    }
    for (std::thread& client : clients) {
        client.join();
    }

    bool all_pong = true;
    for (const std::string& reply : replies) {
        if (reply != "+PONG\r\n") {
            all_pong = false;
        }
    }
    check(all_pong, "every client got its own +PONG");
    check(store.values.empty(), "PING touched no keys");
}

// ---------------------------------------------------------------------------
// Stage 7: the event loop's per-client buffering, without any sockets.
//
// kqueue itself is not unit-tested here. What is testable is the logic the
// event loop wraps around it: bytes accumulate in a per-client input buffer,
// every complete request is executed, replies accumulate in a per-client
// output buffer, and partial writes drain from the front of it.
// ---------------------------------------------------------------------------

// Mirrors handle_readable(): append what "arrived", run every complete request,
// and append each reply to the output buffer.
static void feed(Store& store,
                 std::string& input,
                 std::string& output,
                 const std::string& arrived) {
    input += arrived;

    while (true) {
        const ParseResult result = parse(input);
        if (result.status != ParseStatus::Ok) {
            break;
        }
        output += serialize(execute_command(result.value, store));
        input.erase(0, result.consumed);
    }
}

static void test_input_buffer_partial_reads() {
    std::cout << "\n-- event loop: one request split across several reads --\n";

    Store store;
    std::string input;
    std::string output;

    run(store, {"SET", "foo", "bar"});

    // The request arrives in three pieces, as TCP is entitled to deliver it.
    feed(store, input, output, "*2\r\n$3\r\n");
    check_equal(output, "", "nothing runs on the first fragment");
    check_equal(input, "*2\r\n$3\r\n", "the fragment is held in the input buffer");

    feed(store, input, output, "GET\r\n$3\r\n");
    check_equal(output, "", "still nothing after the second fragment");

    feed(store, input, output, "foo\r\n");
    check_equal(output, "$3\r\nbar\r\n", "the request runs once the last byte arrives");
    check_equal(input, "", "the input buffer is empty again");
}

static void test_input_buffer_pipelining() {
    std::cout << "\n-- event loop: several requests in one read --\n";

    Store store;
    std::string input;
    std::string output;

    feed(store, input, output,
         "*1\r\n$4\r\nPING\r\n"
         "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n"
         "*2\r\n$3\r\nGET\r\n$3\r\nfoo\r\n");

    check_equal(output, "+PONG\r\n+OK\r\n$3\r\nbar\r\n",
                "all three replies are queued in order");
    check_equal(input, "", "the whole read was consumed");
}

static void test_input_buffer_mixed() {
    std::cout << "\n-- event loop: a complete request plus a fragment --\n";

    Store store;
    std::string input;
    std::string output;

    // One whole PING, then the beginning of a GET.
    feed(store, input, output, "*1\r\n$4\r\nPING\r\n*2\r\n$3\r\nGET\r\n");

    check_equal(output, "+PONG\r\n", "the complete request ran");
    check_equal(input, "*2\r\n$3\r\nGET\r\n", "the fragment stayed buffered");

    feed(store, input, output, "$7\r\nmissing\r\n");
    check_equal(output, "+PONG\r\n$-1\r\n", "the second request ran when it completed");
    check_equal(input, "", "the input buffer drained");
}

static void test_output_buffer_partial_writes() {
    std::cout << "\n-- event loop: draining an output buffer in pieces --\n";

    Store store;
    std::string input;
    std::string output;

    // A value large enough that a real socket would not take it in one send().
    const std::string big_value(50000, 'x');
    run(store, {"SET", "big", big_value});
    feed(store, input, output, "*2\r\n$3\r\nGET\r\n$3\r\nbig\r\n");

    const std::string expected = "$50000\r\n" + big_value + "\r\n";
    check(output == expected, "the whole large reply is queued in the output buffer");

    // Simulate send() accepting only part of the buffer each time, which is
    // exactly what a non-blocking socket does when its send buffer fills.
    std::string delivered;
    int write_calls = 0;
    while (!output.empty()) {
        const std::size_t accepted = std::min<std::size_t>(output.size(), 1024);
        delivered.append(output, 0, accepted);
        output.erase(0, accepted);
        ++write_calls;
    }

    check(write_calls > 1, "a large reply really does need several writes");
    check(delivered == expected, "the client receives every byte, in order");
    check(output.empty(), "the output buffer is empty, so write interest can be dropped");
}

static void test_client_state_is_per_fd() {
    std::cout << "\n-- event loop: clients do not share buffers --\n";

    Store store;
    std::unordered_map<int, std::string> input;
    std::unordered_map<int, std::string> output;

    // Client 4 sends half a request, client 5 sends a whole one in between.
    feed(store, input[4], output[4], "*2\r\n$3\r\nGET\r\n");
    feed(store, input[5], output[5], "*1\r\n$4\r\nPING\r\n");

    check_equal(output[5], "+PONG\r\n", "the second client was served immediately");
    check_equal(output[4], "", "the first client's half-request produced nothing");

    // The interleaving did not disturb client 4's buffered fragment.
    feed(store, input[4], output[4], "$7\r\nmissing\r\n");
    check_equal(output[4], "$-1\r\n", "the first client's request completed correctly");
}

// ---------------------------------------------------------------------------
// Stage 8: the append-only log.
//
// Every test uses its own temporary file and removes it afterwards, so none of
// them touch the real redis-lite.aof.
// ---------------------------------------------------------------------------

static const char* const kLogPath = "test-redis-lite.aof";

static void remove_log() {
    std::remove(kLogPath);
}

static std::string read_log() {
    std::ifstream in(kLogPath, std::ios::in | std::ios::binary);
    std::ostringstream contents;
    contents << in.rdbuf();
    return contents.str();
}

static void write_log(const std::string& contents) {
    std::ofstream out(kLogPath, std::ios::out | std::ios::binary | std::ios::trunc);
    out << contents;
}

// Runs one command against `store`, recording it in `log` the way the server does.
static std::string run_logged(Store& store,
                              std::ofstream& log,
                              const std::vector<std::string>& args) {
    std::vector<RespValue> elements;
    for (const std::string& arg : args) {
        elements.push_back(redis_lite::make_bulk_string(arg));
    }
    return serialize(execute_command(redis_lite::make_array(elements), store, &log));
}

static void test_log_records_writes() {
    std::cout << "\n-- persistence: what gets written --\n";

    remove_log();
    Store store;
    std::ofstream log;
    check(redis_lite::open_log(kLogPath, log), "the log file opens");

    run_logged(store, log, {"SET", "foo", "bar"});
    check_equal(read_log(), "SET 3 foo 3 bar\n", "SET is recorded, length-prefixed");

    run_logged(store, log, {"DEL", "foo"});
    check_equal(read_log(), "SET 3 foo 3 bar\nDEL 3 foo\n", "DEL is recorded");

    // A DEL that removed nothing changed no state, so nothing is recorded.
    const std::string before_noop = read_log();
    run_logged(store, log, {"DEL", "nosuchkey"});
    check_equal(read_log(), before_noop, "a DEL that deleted nothing is not recorded");

    remove_log();
}

static void test_log_records_expire() {
    std::cout << "\n-- persistence: EXPIRE is stored as a wall-clock deadline --\n";

    remove_log();
    Store store;
    std::ofstream log;
    redis_lite::open_log(kLogPath, log);

    run_logged(store, log, {"SET", "foo", "bar"});
    run_logged(store, log, {"EXPIRE", "foo", "50"});

    const std::string contents = read_log();
    check(contents.rfind("SET 3 foo 3 bar\nEXPIRE 3 foo ", 0) == 0,
          "EXPIRE is recorded after the SET");

    // The recorded deadline should be roughly 50 seconds from now, in Unix time.
    const std::size_t space = contents.rfind(' ');
    const long long deadline = std::stoll(contents.substr(space + 1));
    const long long now = static_cast<long long>(std::time(nullptr));
    check(deadline >= now + 48 && deadline <= now + 52,
          "the deadline is an absolute Unix timestamp about 50s away");

    // EXPIRE with a non-positive time deletes the key, and records that.
    run_logged(store, log, {"SET", "gone", "value"});
    run_logged(store, log, {"EXPIRE", "gone", "0"});
    const std::string after = read_log();
    check(after.find("DEL 4 gone\n") != std::string::npos,
          "EXPIRE 0 is recorded as a deletion");

    remove_log();
}

static void test_reads_are_not_logged() {
    std::cout << "\n-- persistence: read-only commands are not recorded --\n";

    remove_log();
    Store store;
    std::ofstream log;
    redis_lite::open_log(kLogPath, log);

    run_logged(store, log, {"SET", "foo", "bar"});
    const std::string after_set = read_log();

    run_logged(store, log, {"GET", "foo"});
    check_equal(read_log(), after_set, "GET is not recorded");

    run_logged(store, log, {"TTL", "foo"});
    check_equal(read_log(), after_set, "TTL is not recorded");

    run_logged(store, log, {"PING"});
    check_equal(read_log(), after_set, "PING is not recorded");

    run_logged(store, log, {"GET", "missing"});
    check_equal(read_log(), after_set, "a GET on a missing key is not recorded");

    remove_log();
}

static void test_replay_rebuilds_the_store() {
    std::cout << "\n-- persistence: replay rebuilds the store --\n";

    remove_log();
    {
        Store store;
        std::ofstream log;
        redis_lite::open_log(kLogPath, log);
        run_logged(store, log, {"SET", "name", "Shashank"});
        run_logged(store, log, {"SET", "city", "Hyderabad"});
        run_logged(store, log, {"SET", "gone", "value"});
        run_logged(store, log, {"DEL", "gone"});
        run_logged(store, log, {"EXPIRE", "city", "300"});
    }

    Store restored;
    std::string error;
    check(redis_lite::replay_log(kLogPath, restored, error), "the log replays without error");
    check_equal(error, "", "no error message is produced");

    check_equal(run(restored, {"GET", "name"}), "$8\r\nShashank\r\n", "a SET value is restored");
    check_equal(run(restored, {"GET", "gone"}), "$-1\r\n", "a deleted key stays deleted");
    check_equal(run(restored, {"GET", "city"}), "$9\r\nHyderabad\r\n", "an expiring key is restored");
    check_equal(run(restored, {"TTL", "name"}), ":-1\r\n", "a key with no TTL reports -1");

    const std::string ttl = run(restored, {"TTL", "city"});
    check(ttl == ":300\r\n" || ttl == ":299\r\n", "the restored TTL is close to the original");

    remove_log();
}

static void test_replay_does_not_append() {
    std::cout << "\n-- persistence: replay does not write back into the log --\n";

    remove_log();
    {
        Store store;
        std::ofstream log;
        redis_lite::open_log(kLogPath, log);
        run_logged(store, log, {"SET", "foo", "bar"});
        run_logged(store, log, {"EXPIRE", "foo", "300"});
        run_logged(store, log, {"SET", "other", "value"});
    }

    const std::string before = read_log();

    Store first;
    std::string error;
    redis_lite::replay_log(kLogPath, first, error);
    check_equal(read_log(), before, "one replay leaves the file byte-for-byte identical");

    // Replaying repeatedly must not grow the file either.
    for (int i = 0; i < 5; ++i) {
        Store again;
        redis_lite::replay_log(kLogPath, again, error);
    }
    check_equal(read_log(), before, "five more replays still leave it unchanged");

    remove_log();
}

static void test_replay_ordering_and_ttl_rules() {
    std::cout << "\n-- persistence: ordering, and SET clearing an old TTL --\n";

    remove_log();
    {
        Store store;
        std::ofstream log;
        redis_lite::open_log(kLogPath, log);

        // Later writes must win.
        run_logged(store, log, {"SET", "counter", "1"});
        run_logged(store, log, {"SET", "counter", "2"});
        run_logged(store, log, {"SET", "counter", "3"});

        // SET after EXPIRE must clear the expiration.
        run_logged(store, log, {"SET", "foo", "bar"});
        run_logged(store, log, {"EXPIRE", "foo", "300"});
        run_logged(store, log, {"SET", "foo", "new"});

        // DEL after EXPIRE must leave nothing behind.
        run_logged(store, log, {"SET", "temp", "value"});
        run_logged(store, log, {"EXPIRE", "temp", "300"});
        run_logged(store, log, {"DEL", "temp"});
    }

    Store restored;
    std::string error;
    check(redis_lite::replay_log(kLogPath, restored, error), "the log replays");

    check_equal(run(restored, {"GET", "counter"}), "$1\r\n3\r\n", "the last SET wins");
    check_equal(run(restored, {"GET", "foo"}), "$3\r\nnew\r\n", "the rewritten value is restored");
    check_equal(run(restored, {"TTL", "foo"}), ":-1\r\n", "SET cleared the old TTL across a restart");
    check_equal(run(restored, {"GET", "temp"}), "$-1\r\n", "the deleted key is gone");
    check_equal(run(restored, {"TTL", "temp"}), ":-2\r\n", "no stale expiration is left behind");

    // A stale deadline must not follow a key that is later recreated.
    check_equal(run(restored, {"SET", "temp", "fresh"}), "+OK\r\n", "the key can be recreated");
    check_equal(run(restored, {"TTL", "temp"}), ":-1\r\n", "the recreated key has no TTL");

    remove_log();
}

static void test_replay_drops_expired_keys() {
    std::cout << "\n-- persistence: a key whose deadline passed while down --\n";

    // Written by hand so the test is deterministic: no sleeping required.
    const long long now = static_cast<long long>(std::time(nullptr));
    write_log("SET 5 alive 1 a\n"
              "EXPIRE 5 alive " + std::to_string(now + 3600) + "\n"
              "SET 4 dead 1 d\n"
              "EXPIRE 4 dead " + std::to_string(now - 60) + "\n");

    Store restored;
    std::string error;
    check(redis_lite::replay_log(kLogPath, restored, error), "the log replays");

    check_equal(run(restored, {"GET", "alive"}), "$1\r\na\r\n", "a key still inside its deadline survives");
    check_equal(run(restored, {"GET", "dead"}), "$-1\r\n", "a key past its deadline is gone");
    check_equal(run(restored, {"TTL", "dead"}), ":-2\r\n", "the expired key reports -2");
    check(restored.values.count("dead") == 0, "the expired key is not in the store at all");

    remove_log();
}

static void test_missing_and_empty_logs() {
    std::cout << "\n-- persistence: missing and empty log files --\n";

    remove_log();
    Store missing;
    std::string error;
    check(redis_lite::replay_log(kLogPath, missing, error), "a missing log file is not an error");
    check(missing.values.empty(), "a missing log gives an empty store");
    check_equal(error, "", "no error message for a missing file");

    write_log("");
    Store empty;
    check(redis_lite::replay_log(kLogPath, empty, error), "an empty log file is not an error");
    check(empty.values.empty(), "an empty log gives an empty store");

    remove_log();
}

static void test_malformed_log_is_refused() {
    std::cout << "\n-- persistence: malformed data is refused, not half-applied --\n";

    const std::vector<std::pair<std::string, std::string>> broken = {
        {"NONSENSE 3 foo\n",                 "an unknown record type"},
        {"SET 3 foo\n",                      "a SET missing its value"},
        {"SET 99 foo 3 bar\n",               "a length longer than the data"},
        {"SET 999999999999 x 3 bar\n",       "a length larger than the whole file"},
        {"SET 3 foo 999999999999 x\n",       "a huge length on the value field"},
        {"SET 3 foo 3 bar",                   "a record with no trailing newline"},
        {"SET x foo 3 bar\n",                "a non-numeric length"},
        {"EXPIRE 3 foo notanumber\n",        "a non-numeric deadline"},
        {"SET 3 foo 3 bar\nDEL\n",           "a truncated second record"},
    };

    for (const auto& entry : broken) {
        write_log(entry.first);

        // Pre-populate, to prove a failed replay leaves the store untouched.
        Store store;
        run(store, {"SET", "existing", "value"});

        std::string error;
        const bool ok = redis_lite::replay_log(kLogPath, store, error);

        check(!ok, std::string("replay refuses ") + entry.second);
        check(!error.empty(), std::string("an error is reported for ") + entry.second);
        check_equal(run(store, {"GET", "existing"}), "$5\r\nvalue\r\n",
                    std::string("the store is untouched after ") + entry.second);
    }

    remove_log();
}

// The store now holds a variant, so a test that wants the string behind a key
// has to ask for that alternative.
static std::string string_at(Store& store, const std::string& key) {
    const auto found = store.values.find(key);
    if (found == store.values.end()) {
        return "<missing>";
    }
    const redis_lite::StringValue* text = std::get_if<redis_lite::StringValue>(&found->second);
    return (text == nullptr) ? "<not a string>" : *text;
}

static void test_binary_safe_keys_and_values() {
    std::cout << "\n-- persistence: awkward keys and values survive --\n";

    const std::string spaced_key = "key with spaces";
    const std::string newline_value = "line one\nline two\r\nline three";
    const std::string empty_value;
    const std::string numeric_looking = "42 7";

    remove_log();
    {
        Store store;
        std::ofstream log;
        redis_lite::open_log(kLogPath, log);
        run_logged(store, log, {"SET", spaced_key, newline_value});
        run_logged(store, log, {"SET", "empty", empty_value});
        run_logged(store, log, {"SET", "tricky", numeric_looking});
        run_logged(store, log, {"SET", "SET 3 foo", "looks like a record"});
    }

    Store restored;
    std::string error;
    check(redis_lite::replay_log(kLogPath, restored, error), "the log replays");
    check_equal(error, "", "no error for awkward data");

    check_equal(string_at(restored, spaced_key), newline_value,
                "a key with spaces and a value with newlines round-trip");
    check(restored.values.count("empty") == 1 && string_at(restored, "empty").empty(),
          "an empty value round-trips");
    check_equal(string_at(restored, "tricky"), numeric_looking,
                "a value that looks like a length prefix round-trips");
    check_equal(string_at(restored, "SET 3 foo"), "looks like a record",
                "a key that looks like a whole record round-trips");

    remove_log();
}

// ---------------------------------------------------------------------------
// Stage 9: lists, hashes and sets.
// ---------------------------------------------------------------------------

static bool is_wrong_type(const std::string& reply) {
    return reply.rfind("-WRONGTYPE ", 0) == 0;
}

// Parses an array reply and returns its elements sorted, so tests never depend
// on the iteration order of an unordered_set.
static std::string sorted_members(const std::string& serialized) {
    const ParseResult result = parse(serialized);
    if (result.status != ParseStatus::Ok || result.value.type != RespType::Array) {
        return "<not an array>";
    }

    std::vector<std::string> members;
    for (const RespValue& element : result.value.elements) {
        members.push_back(element.string);
    }
    std::sort(members.begin(), members.end());

    std::string joined;
    for (const std::string& member : members) {
        joined += member;
        joined += "|";
    }
    return joined;
}

// Elements of an array reply in the order the server sent them (lists care).
static std::string ordered_elements(const std::string& serialized) {
    const ParseResult result = parse(serialized);
    if (result.status != ParseStatus::Ok || result.value.type != RespType::Array) {
        return "<not an array>";
    }

    std::string joined;
    for (const RespValue& element : result.value.elements) {
        joined += element.string;
        joined += "|";
    }
    return joined;
}

static void test_lists() {
    std::cout << "\n-- lists --\n";

    Store store;

    check_equal(run(store, {"LLEN", "mylist"}), ":0\r\n", "LLEN of a missing key is 0");
    check_equal(run(store, {"LPOP", "mylist"}), "$-1\r\n", "LPOP of a missing key is nil");
    check_equal(run(store, {"RPOP", "mylist"}), "$-1\r\n", "RPOP of a missing key is nil");
    check_equal(ordered_elements(run(store, {"LRANGE", "mylist", "0", "-1"})), "",
                "LRANGE of a missing key is empty");

    check_equal(run(store, {"RPUSH", "mylist", "b"}), ":1\r\n", "RPUSH creates the list");
    check_equal(run(store, {"RPUSH", "mylist", "c"}), ":2\r\n", "RPUSH appends on the right");
    check_equal(run(store, {"LPUSH", "mylist", "a"}), ":3\r\n", "LPUSH prepends on the left");
    check_equal(run(store, {"LLEN", "mylist"}), ":3\r\n", "LLEN counts the elements");
    check_equal(ordered_elements(run(store, {"LRANGE", "mylist", "0", "-1"})), "a|b|c|",
                "the list reads a, b, c in order");

    check_equal(run(store, {"LPOP", "mylist"}), "$1\r\na\r\n", "LPOP takes from the left");
    check_equal(run(store, {"RPOP", "mylist"}), "$1\r\nc\r\n", "RPOP takes from the right");
    check_equal(run(store, {"LLEN", "mylist"}), ":1\r\n", "one element is left");

    // Emptying a list removes the key, exactly as Redis does.
    check_equal(run(store, {"LPOP", "mylist"}), "$1\r\nb\r\n", "the last element pops");
    check_equal(run(store, {"LLEN", "mylist"}), ":0\r\n", "the emptied list reports length 0");
    check(store.values.count("mylist") == 0, "the emptied list is removed from the store");
    check_equal(run(store, {"TTL", "mylist"}), ":-2\r\n", "the emptied key reports -2");

    // Repeated operations, and duplicate values, are fine.
    for (int i = 0; i < 5; ++i) {
        run(store, {"RPUSH", "repeat", "same"});
    }
    check_equal(run(store, {"LLEN", "repeat"}), ":5\r\n", "duplicates are kept, not deduplicated");
    check_equal(ordered_elements(run(store, {"LRANGE", "repeat", "0", "-1"})),
                "same|same|same|same|same|", "all five duplicates are present");

    // Arity.
    check(run(store, {"LPUSH"}).rfind("-ERR wrong number", 0) == 0, "LPUSH needs arguments");
    check(run(store, {"LPUSH", "k"}).rfind("-ERR wrong number", 0) == 0, "LPUSH needs a value");
    check(run(store, {"LPOP", "k", "extra"}).rfind("-ERR wrong number", 0) == 0,
          "LPOP takes exactly one key");
    check(run(store, {"LRANGE", "k", "0"}).rfind("-ERR wrong number", 0) == 0,
          "LRANGE needs start and stop");
    check(run(store, {"LRANGE", "k", "a", "b"}).rfind("-ERR value is not an integer", 0) == 0,
          "LRANGE rejects non-numeric bounds");
}

static void test_list_ranges() {
    std::cout << "\n-- LRANGE, including negative indices --\n";

    Store store;
    for (const std::string& value : {"a", "b", "c", "d", "e"}) {
        run(store, {"RPUSH", "letters", value});
    }

    check_equal(ordered_elements(run(store, {"LRANGE", "letters", "0", "-1"})), "a|b|c|d|e|",
                "0 to -1 is the whole list");
    check_equal(ordered_elements(run(store, {"LRANGE", "letters", "0", "0"})), "a|",
                "0 to 0 is the first element");
    check_equal(ordered_elements(run(store, {"LRANGE", "letters", "1", "3"})), "b|c|d|",
                "a middle range");
    check_equal(ordered_elements(run(store, {"LRANGE", "letters", "-2", "-1"})), "d|e|",
                "negative indices count from the end");
    check_equal(ordered_elements(run(store, {"LRANGE", "letters", "-100", "100"})), "a|b|c|d|e|",
                "out-of-range bounds are clamped");
    check_equal(ordered_elements(run(store, {"LRANGE", "letters", "3", "1"})), "",
                "a reversed range is empty");
    check_equal(ordered_elements(run(store, {"LRANGE", "letters", "10", "20"})), "",
                "a range past the end is empty");
}

static void test_hashes() {
    std::cout << "\n-- hashes --\n";

    Store store;

    check_equal(run(store, {"HLEN", "user"}), ":0\r\n", "HLEN of a missing key is 0");
    check_equal(run(store, {"HGET", "user", "name"}), "$-1\r\n", "HGET of a missing key is nil");
    check_equal(run(store, {"HEXISTS", "user", "name"}), ":0\r\n",
                "HEXISTS on a missing key is 0");
    check_equal(run(store, {"HDEL", "user", "name"}), ":0\r\n", "HDEL on a missing key is 0");

    check_equal(run(store, {"HSET", "user", "name", "Shashank"}), ":1\r\n",
                "HSET returns 1 for a new field");
    check_equal(run(store, {"HSET", "user", "city", "Hyderabad"}), ":1\r\n",
                "a second new field also returns 1");
    check_equal(run(store, {"HSET", "user", "name", "Shash"}), ":0\r\n",
                "HSET returns 0 when updating an existing field");

    check_equal(run(store, {"HGET", "user", "name"}), "$5\r\nShash\r\n",
                "the updated value is returned");
    check_equal(run(store, {"HGET", "user", "city"}), "$9\r\nHyderabad\r\n",
                "the other field is untouched");
    check_equal(run(store, {"HGET", "user", "missing"}), "$-1\r\n",
                "a missing field is nil");
    check_equal(run(store, {"HLEN", "user"}), ":2\r\n", "HLEN counts the fields");
    check_equal(run(store, {"HEXISTS", "user", "name"}), ":1\r\n", "HEXISTS finds a field");
    check_equal(run(store, {"HEXISTS", "user", "missing"}), ":0\r\n",
                "HEXISTS does not find a missing field");

    check_equal(run(store, {"HDEL", "user", "city"}), ":1\r\n", "HDEL removes a field");
    check_equal(run(store, {"HDEL", "user", "city"}), ":0\r\n",
                "HDEL on an already-removed field is 0");
    check_equal(run(store, {"HLEN", "user"}), ":1\r\n", "the field count drops");

    // Emptying a hash removes the key.
    check_equal(run(store, {"HDEL", "user", "name"}), ":1\r\n", "the last field is removed");
    check(store.values.count("user") == 0, "the emptied hash is removed from the store");
    check_equal(run(store, {"HLEN", "user"}), ":0\r\n", "the emptied hash reports length 0");

    // Fields and values may contain anything.
    run(store, {"HSET", "odd", "field with spaces", "value\r\nwith newlines"});
    check_equal(run(store, {"HGET", "odd", "field with spaces"}),
                "$20\r\nvalue\r\nwith newlines\r\n", "awkward fields and values work");

    check(run(store, {"HSET", "k", "f"}).rfind("-ERR wrong number", 0) == 0,
          "HSET needs field and value");
    check(run(store, {"HGET", "k"}).rfind("-ERR wrong number", 0) == 0, "HGET needs a field");
    check(run(store, {"HLEN"}).rfind("-ERR wrong number", 0) == 0, "HLEN needs a key");
}

static void test_sets() {
    std::cout << "\n-- sets --\n";

    Store store;

    check_equal(run(store, {"SCARD", "tags"}), ":0\r\n", "SCARD of a missing key is 0");
    check_equal(run(store, {"SISMEMBER", "tags", "a"}), ":0\r\n",
                "SISMEMBER on a missing key is 0");
    check_equal(run(store, {"SREM", "tags", "a"}), ":0\r\n", "SREM on a missing key is 0");
    check_equal(sorted_members(run(store, {"SMEMBERS", "tags"})), "",
                "SMEMBERS of a missing key is empty");

    check_equal(run(store, {"SADD", "tags", "red"}), ":1\r\n", "SADD adds a new member");
    check_equal(run(store, {"SADD", "tags", "green"}), ":1\r\n", "SADD adds a second member");
    check_equal(run(store, {"SADD", "tags", "red"}), ":0\r\n",
                "SADD returns 0 for a duplicate");
    check_equal(run(store, {"SCARD", "tags"}), ":2\r\n", "the duplicate did not grow the set");

    check_equal(sorted_members(run(store, {"SMEMBERS", "tags"})), "green|red|",
                "SMEMBERS returns both members");
    check_equal(run(store, {"SISMEMBER", "tags", "red"}), ":1\r\n", "SISMEMBER finds a member");
    check_equal(run(store, {"SISMEMBER", "tags", "blue"}), ":0\r\n",
                "SISMEMBER does not find a non-member");

    check_equal(run(store, {"SREM", "tags", "red"}), ":1\r\n", "SREM removes a member");
    check_equal(run(store, {"SREM", "tags", "red"}), ":0\r\n",
                "SREM on an already-removed member is 0");
    check_equal(run(store, {"SCARD", "tags"}), ":1\r\n", "the cardinality drops");

    // Emptying a set removes the key.
    check_equal(run(store, {"SREM", "tags", "green"}), ":1\r\n", "the last member is removed");
    check(store.values.count("tags") == 0, "the emptied set is removed from the store");
    check_equal(run(store, {"SCARD", "tags"}), ":0\r\n", "the emptied set reports 0");

    check(run(store, {"SADD", "k"}).rfind("-ERR wrong number", 0) == 0, "SADD needs a member");
    check(run(store, {"SCARD"}).rfind("-ERR wrong number", 0) == 0, "SCARD needs a key");
    check(run(store, {"SMEMBERS", "k", "extra"}).rfind("-ERR wrong number", 0) == 0,
          "SMEMBERS takes exactly one key");
}

static void test_wrong_type_errors() {
    std::cout << "\n-- WRONGTYPE: one key holds exactly one type --\n";

    const std::vector<std::vector<std::string>> string_commands = {
        {"GET", "k"},
    };
    const std::vector<std::vector<std::string>> list_commands = {
        {"LPUSH", "k", "v"}, {"RPUSH", "k", "v"}, {"LPOP", "k"},
        {"RPOP", "k"}, {"LLEN", "k"}, {"LRANGE", "k", "0", "-1"},
    };
    const std::vector<std::vector<std::string>> hash_commands = {
        {"HSET", "k", "f", "v"}, {"HGET", "k", "f"}, {"HDEL", "k", "f"},
        {"HEXISTS", "k", "f"}, {"HLEN", "k"},
    };
    const std::vector<std::vector<std::string>> set_commands = {
        {"SADD", "k", "m"}, {"SREM", "k", "m"}, {"SISMEMBER", "k", "m"},
        {"SCARD", "k"}, {"SMEMBERS", "k"},
    };

    struct Holder {
        std::vector<std::string> create;
        std::string description;
        std::vector<std::vector<std::vector<std::string>>> foreign;
    };

    const std::vector<Holder> holders = {
        {{"SET", "k", "text"},        "a string", {list_commands, hash_commands, set_commands}},
        {{"RPUSH", "k", "item"},      "a list",   {string_commands, hash_commands, set_commands}},
        {{"HSET", "k", "f", "v"},     "a hash",   {string_commands, list_commands, set_commands}},
        {{"SADD", "k", "m"},          "a set",    {string_commands, list_commands, hash_commands}},
    };

    for (const Holder& holder : holders) {
        int rejected = 0;
        int attempted = 0;

        for (const auto& family : holder.foreign) {
            for (const std::vector<std::string>& command : family) {
                Store store;
                run(store, holder.create);

                ++attempted;
                if (is_wrong_type(run(store, command))) {
                    ++rejected;
                }
            }
        }

        check(rejected == attempted,
              "every foreign command against " + holder.description + " returns WRONGTYPE (" +
                  std::to_string(rejected) + "/" + std::to_string(attempted) + ")");
    }

    // A rejected command must not create or damage anything.
    Store store;
    run(store, {"SET", "k", "text"});
    check(is_wrong_type(run(store, {"LPUSH", "k", "v"})), "LPUSH on a string is rejected");
    check_equal(run(store, {"GET", "k"}), "$4\r\ntext\r\n", "the string is unharmed");
    check(store.values.size() == 1, "no extra key was created");

    check(is_wrong_type(run(store, {"HSET", "k", "f", "v"})), "HSET on a string is rejected");
    check(store.values.size() == 1, "a rejected HSET creates nothing");

    // DEL and TTL are type-agnostic and must keep working.
    Store mixed;
    run(mixed, {"RPUSH", "l", "x"});
    run(mixed, {"HSET", "h", "f", "v"});
    run(mixed, {"SADD", "s", "m"});
    check_equal(run(mixed, {"DEL", "l"}), ":1\r\n", "DEL removes a list");
    check_equal(run(mixed, {"DEL", "h"}), ":1\r\n", "DEL removes a hash");
    check_equal(run(mixed, {"DEL", "s"}), ":1\r\n", "DEL removes a set");
    check(mixed.values.empty(), "DEL works on every type");
}

static void test_ttl_with_every_type() {
    std::cout << "\n-- TTL applies to every value type --\n";

    Store store;
    run(store, {"SET", "s", "text"});
    run(store, {"RPUSH", "l", "item"});
    run(store, {"HSET", "h", "f", "v"});
    run(store, {"SADD", "t", "m"});

    for (const std::string& key : {"s", "l", "h", "t"}) {
        check_equal(run(store, {"TTL", key}), ":-1\r\n", "TTL is -1 for key '" + key + "'");
        check_equal(run(store, {"EXPIRE", key, "60"}), ":1\r\n",
                    "EXPIRE works on key '" + key + "'");
        check_equal(run(store, {"TTL", key}), ":60\r\n", "TTL reports 60 for key '" + key + "'");
    }

    // EXPIRE 0 deletes, whatever the type.
    for (const std::string& key : {"l", "h", "t"}) {
        check_equal(run(store, {"EXPIRE", key, "0"}), ":1\r\n",
                    "EXPIRE 0 succeeds on key '" + key + "'");
        check_equal(run(store, {"TTL", key}), ":-2\r\n", "key '" + key + "' is gone");
        check(store.values.count(key) == 0, "key '" + key + "' left nothing behind");
    }

    // And a real deadline elapsing works for a collection too.
    Store timed;
    run(timed, {"RPUSH", "list", "a"});
    run(timed, {"RPUSH", "list", "b"});
    run(timed, {"EXPIRE", "list", "1"});
    check_equal(run(timed, {"LLEN", "list"}), ":2\r\n", "the list is alive before its deadline");

    sleep_ms(1200);

    check_equal(run(timed, {"LLEN", "list"}), ":0\r\n", "the expired list reads as empty");
    check_equal(run(timed, {"LRANGE", "list", "0", "-1"}), "*0\r\n",
                "LRANGE on the expired list is empty");
    check(timed.values.count("list") == 0, "the expired list was actually removed");
}

static void test_replacement_semantics() {
    std::cout << "\n-- replacing a key with another type --\n";

    Store store;

    // SET overwrites any type, and clears the TTL along with it.
    run(store, {"RPUSH", "k", "a"});
    run(store, {"RPUSH", "k", "b"});
    run(store, {"EXPIRE", "k", "100"});
    check_equal(run(store, {"TTL", "k"}), ":100\r\n", "the list has a TTL");

    check_equal(run(store, {"SET", "k", "now a string"}), "+OK\r\n", "SET replaces the list");
    check_equal(run(store, {"GET", "k"}), "$12\r\nnow a string\r\n", "the string is readable");
    check(is_wrong_type(run(store, {"LLEN", "k"})), "the key is no longer a list");
    check_equal(run(store, {"TTL", "k"}), ":-1\r\n", "SET cleared the list's TTL");

    // Other types are never replaced implicitly; DEL first.
    check(is_wrong_type(run(store, {"RPUSH", "k", "x"})), "RPUSH will not replace a string");
    check_equal(run(store, {"DEL", "k"}), ":1\r\n", "DEL removes it");
    check_equal(run(store, {"RPUSH", "k", "x"}), ":1\r\n", "the key can be recreated as a list");
    check_equal(run(store, {"LLEN", "k"}), ":1\r\n", "and behaves as a list");

    // Emptying a collection frees the name for a different type.
    run(store, {"LPOP", "k"});
    check(store.values.count("k") == 0, "the emptied list released the key");
    check_equal(run(store, {"SADD", "k", "m"}), ":1\r\n", "the name is reusable as a set");
    check_equal(run(store, {"SCARD", "k"}), ":1\r\n", "and behaves as a set");

    // A stale TTL must not follow a key into its next life.
    Store second;
    run(second, {"HSET", "h", "f", "v"});
    run(second, {"EXPIRE", "h", "100"});
    run(second, {"DEL", "h"});
    run(second, {"SADD", "h", "m"});
    check_equal(run(second, {"TTL", "h"}), ":-1\r\n",
                "the recreated key has no leftover expiration");
}

static void test_persistence_of_collections() {
    std::cout << "\n-- persistence: lists, hashes and sets --\n";

    remove_log();
    {
        Store store;
        std::ofstream log;
        redis_lite::open_log(kLogPath, log);

        run_logged(store, log, {"RPUSH", "list", "b"});
        run_logged(store, log, {"LPUSH", "list", "a"});
        run_logged(store, log, {"RPUSH", "list", "c"});
        run_logged(store, log, {"HSET", "hash", "field", "value"});
        run_logged(store, log, {"HSET", "hash", "other", "thing"});
        run_logged(store, log, {"SADD", "set", "x"});
        run_logged(store, log, {"SADD", "set", "y"});
        run_logged(store, log, {"SADD", "set", "x"});  // duplicate: no state change
    }

    const std::string contents = read_log();
    check(contents.find("RPUSH 4 list 1 b\n") != std::string::npos, "RPUSH is recorded");
    check(contents.find("LPUSH 4 list 1 a\n") != std::string::npos, "LPUSH is recorded");
    check(contents.find("HSET 4 hash 5 field 5 value\n") != std::string::npos,
          "HSET is recorded with field and value");
    check(contents.find("SADD 3 set 1 x\n") != std::string::npos, "SADD is recorded");

    // The duplicate SADD changed nothing, so it must not appear a second time.
    std::size_t sadd_x_count = 0;
    for (std::size_t at = contents.find("SADD 3 set 1 x\n");
         at != std::string::npos;
         at = contents.find("SADD 3 set 1 x\n", at + 1)) {
        ++sadd_x_count;
    }
    check(sadd_x_count == 1, "a duplicate SADD is not recorded");

    Store restored;
    std::string error;
    check(redis_lite::replay_log(kLogPath, restored, error), "the log replays");
    check_equal(error, "", "no error replaying collections");

    check_equal(ordered_elements(run(restored, {"LRANGE", "list", "0", "-1"})), "a|b|c|",
                "the list is restored in the right order");
    check_equal(run(restored, {"HGET", "hash", "field"}), "$5\r\nvalue\r\n",
                "a hash field is restored");
    check_equal(run(restored, {"HLEN", "hash"}), ":2\r\n", "both hash fields are restored");
    check_equal(sorted_members(run(restored, {"SMEMBERS", "set"})), "x|y|",
                "the set members are restored");
    check_equal(run(restored, {"SCARD", "set"}), ":2\r\n", "the set has the right size");

    remove_log();
}

static void test_persistence_of_collection_removals() {
    std::cout << "\n-- persistence: pops and removals replay correctly --\n";

    remove_log();
    {
        Store store;
        std::ofstream log;
        redis_lite::open_log(kLogPath, log);

        for (const std::string& value : {"a", "b", "c", "d"}) {
            run_logged(store, log, {"RPUSH", "list", value});
        }
        run_logged(store, log, {"LPOP", "list"});   // drops "a"
        run_logged(store, log, {"RPOP", "list"});   // drops "d"

        run_logged(store, log, {"HSET", "hash", "keep", "1"});
        run_logged(store, log, {"HSET", "hash", "drop", "2"});
        run_logged(store, log, {"HDEL", "hash", "drop"});
        run_logged(store, log, {"HDEL", "hash", "absent"});  // no state change

        run_logged(store, log, {"SADD", "set", "keep"});
        run_logged(store, log, {"SADD", "set", "drop"});
        run_logged(store, log, {"SREM", "set", "drop"});
        run_logged(store, log, {"SREM", "set", "absent"});  // no state change
    }

    const std::string contents = read_log();
    check(contents.find("LPOP 4 list\n") != std::string::npos, "LPOP is recorded");
    check(contents.find("RPOP 4 list\n") != std::string::npos, "RPOP is recorded");
    check(contents.find("HDEL 4 hash 4 drop\n") != std::string::npos, "HDEL is recorded");
    check(contents.find("HDEL 4 hash 6 absent\n") == std::string::npos,
          "an HDEL that removed nothing is not recorded");
    check(contents.find("SREM 3 set 4 drop\n") != std::string::npos, "SREM is recorded");
    check(contents.find("SREM 3 set 6 absent\n") == std::string::npos,
          "an SREM that removed nothing is not recorded");

    Store restored;
    std::string error;
    check(redis_lite::replay_log(kLogPath, restored, error), "the log replays");

    check_equal(ordered_elements(run(restored, {"LRANGE", "list", "0", "-1"})), "b|c|",
                "the popped elements are gone after replay");
    check_equal(run(restored, {"HLEN", "hash"}), ":1\r\n", "the deleted hash field stays deleted");
    check_equal(run(restored, {"HEXISTS", "hash", "drop"}), ":0\r\n", "HDEL survived the restart");
    check_equal(sorted_members(run(restored, {"SMEMBERS", "set"})), "keep|",
                "the removed set member stays removed");

    remove_log();
}

static void test_persistence_emptied_collections() {
    std::cout << "\n-- persistence: a collection emptied to nothing --\n";

    remove_log();
    {
        Store store;
        std::ofstream log;
        redis_lite::open_log(kLogPath, log);

        run_logged(store, log, {"RPUSH", "gone", "only"});
        run_logged(store, log, {"LPOP", "gone"});          // empties, so the key is removed
        run_logged(store, log, {"SADD", "gone", "member"}); // the name is reused as a set
    }

    Store restored;
    std::string error;
    check(redis_lite::replay_log(kLogPath, restored, error), "the log replays");
    check_equal(run(restored, {"SCARD", "gone"}), ":1\r\n",
                "the key came back as a set, not a list");
    check(is_wrong_type(run(restored, {"LLEN", "gone"})),
          "the old list type did not survive");

    remove_log();
}

static void test_persistence_collection_ttl() {
    std::cout << "\n-- persistence: a TTL on a collection --\n";

    remove_log();
    {
        Store store;
        std::ofstream log;
        redis_lite::open_log(kLogPath, log);

        run_logged(store, log, {"RPUSH", "alive", "x"});
        run_logged(store, log, {"EXPIRE", "alive", "600"});
        run_logged(store, log, {"HSET", "doomed", "f", "v"});
    }

    // Give the doomed hash a deadline that already passed.
    const long long now = static_cast<long long>(std::time(nullptr));
    std::ofstream append(kLogPath, std::ios::app | std::ios::binary);
    append << "EXPIRE 6 doomed " << (now - 60) << "\n";
    append.close();

    Store restored;
    std::string error;
    check(redis_lite::replay_log(kLogPath, restored, error), "the log replays");

    check_equal(run(restored, {"LLEN", "alive"}), ":1\r\n", "the list survived the restart");
    const std::string ttl = run(restored, {"TTL", "alive"});
    check(ttl == ":600\r\n" || ttl == ":599\r\n", "its TTL came back roughly intact");

    check_equal(run(restored, {"HLEN", "doomed"}), ":0\r\n", "the expired hash is gone");
    check(restored.values.count("doomed") == 0, "the expired hash left nothing behind");

    remove_log();
}

static void test_persistence_replay_is_still_read_only() {
    std::cout << "\n-- persistence: replaying collections writes nothing back --\n";

    remove_log();
    {
        Store store;
        std::ofstream log;
        redis_lite::open_log(kLogPath, log);
        run_logged(store, log, {"RPUSH", "list", "a"});
        run_logged(store, log, {"HSET", "hash", "f", "v"});
        run_logged(store, log, {"SADD", "set", "m"});
        run_logged(store, log, {"LPOP", "list"});
    }

    const std::string before = read_log();
    for (int i = 0; i < 5; ++i) {
        Store scratch;
        std::string error;
        redis_lite::replay_log(kLogPath, scratch, error);
    }
    check_equal(read_log(), before, "five replays leave the log byte-for-byte identical");

    remove_log();
}

static void test_persistence_malformed_collection_records() {
    std::cout << "\n-- persistence: malformed collection records are refused --\n";

    const std::vector<std::pair<std::string, std::string>> broken = {
        {"HSET 4 hash 5 field\n",        "an HSET missing its value"},
        {"LPUSH 4 list\n",               "an LPUSH missing its value"},
        {"SADD 3 set\n",                 "an SADD missing its member"},
        {"HSET 4 hash 99 field 1 v\n",   "an HSET field length past the end"},
        {"HSET 4 hash 999999999999 f 1 v\n", "a huge declared field length"},
        {"LPOP 4 list 1 x\n",            "an LPOP with a stray extra argument"},
        {"ZADD 3 key 1 m\n",             "a verb we do not implement"},
    };

    for (const auto& entry : broken) {
        write_log(entry.first);

        Store store;
        run(store, {"SET", "existing", "value"});

        std::string error;
        const bool ok = redis_lite::replay_log(kLogPath, store, error);

        check(!ok, std::string("replay refuses ") + entry.second);
        check(!error.empty(), std::string("an error is reported for ") + entry.second);
        check_equal(run(store, {"GET", "existing"}), "$5\r\nvalue\r\n",
                    std::string("the store is untouched after ") + entry.second);
    }

    remove_log();
}

static void test_persistence_awkward_collection_data() {
    std::cout << "\n-- persistence: awkward collection data survives --\n";

    remove_log();
    {
        Store store;
        std::ofstream log;
        redis_lite::open_log(kLogPath, log);
        run_logged(store, log, {"RPUSH", "list", "an item with spaces"});
        run_logged(store, log, {"RPUSH", "list", "an item\nwith a newline"});
        run_logged(store, log, {"HSET", "hash", "field with spaces", "HSET 1 a 1 b"});
        run_logged(store, log, {"SADD", "set", "member with spaces"});
    }

    Store restored;
    std::string error;
    check(redis_lite::replay_log(kLogPath, restored, error), "the log replays");
    check_equal(error, "", "no error for awkward collection data");

    check_equal(ordered_elements(run(restored, {"LRANGE", "list", "0", "-1"})),
                "an item with spaces|an item\nwith a newline|",
                "list items with spaces and newlines round-trip");
    check_equal(run(restored, {"HGET", "hash", "field with spaces"}),
                "$12\r\nHSET 1 a 1 b\r\n",
                "a hash field with spaces, holding record-like text, round-trips");
    check_equal(sorted_members(run(restored, {"SMEMBERS", "set"})), "member with spaces|",
                "a set member with spaces round-trips");

    remove_log();
}

// Builds `levels` nested single-element arrays wrapping an integer leaf.
static std::string nested_array(int levels) {
    std::string out;
    out.reserve(static_cast<std::size_t>(levels) * 4 + 4);
    for (int i = 0; i < levels; ++i) {
        out += "*1\r\n";
    }
    out += ":1\r\n";
    return out;
}

static void test_nesting_depth_limit() {
    std::cout << "\n-- RESP nesting depth --\n";

    check_status(nested_array(4), ParseStatus::Ok, "a few levels of nesting parse");
    check_status(nested_array(127), ParseStatus::Ok, "nesting at the limit still parses");
    check_status(nested_array(128), ParseStatus::Malformed,
                 "one level past the limit is malformed");

    // These depths used to exhaust the stack and take the server down.
    check_status(nested_array(20000), ParseStatus::Malformed,
                 "20000 levels is malformed, not a crash");
    check_status(nested_array(50000), ParseStatus::Malformed,
                 "50000 levels is malformed, not a crash");

    const ParseResult deep = parse(nested_array(20000));
    check(!deep.error.empty(), "the depth failure explains itself");

    // The limit must not disturb ordinary nested replies.
    check_round_trip("*2\r\n*2\r\n:1\r\n:2\r\n+OK\r\n", "an ordinary nested array");
}

static void test_expire_rejects_unrepresentable_deadlines() {
    std::cout << "\n-- EXPIRE range --\n";

    Store store;
    run(store, {"SET", "k", "v"});

    // The clock cannot hold a deadline further out than this.
    const long long max_seconds = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::time_point::max() -
        std::chrono::steady_clock::now()).count();

    check(run(store, {"EXPIRE", "k", std::to_string(max_seconds + 1)})
              .rfind("-ERR value is not an integer", 0) == 0,
          "one second past the representable range is rejected");
    check_equal(run(store, {"GET", "k"}), "$1\r\nv\r\n", "the rejected EXPIRE left the key alone");
    check_equal(run(store, {"TTL", "k"}), ":-1\r\n", "and set no expiration");

    check(run(store, {"EXPIRE", "k", "9223372036854775807"})
              .rfind("-ERR value is not an integer", 0) == 0,
          "the largest long long is rejected");
    check_equal(run(store, {"GET", "k"}), "$1\r\nv\r\n", "the key survived that too");
    check_equal(run(store, {"TTL", "k"}), ":-1\r\n", "still no expiration");

    check_equal(run(store, {"EXPIRE", "k", std::to_string(max_seconds - 60)}), ":1\r\n",
                "a value just inside the range is accepted");
    check(run(store, {"TTL", "k"}).rfind(":", 0) == 0, "TTL reports a number for it");
    check_equal(run(store, {"GET", "k"}), "$1\r\nv\r\n", "the key is still readable");

    // Zero and negative keep deleting, exactly as before.
    check_equal(run(store, {"EXPIRE", "k", "0"}), ":1\r\n", "EXPIRE 0 still deletes");
    check_equal(run(store, {"GET", "k"}), "$-1\r\n", "the key is gone");

    run(store, {"SET", "k2", "v"});
    check_equal(run(store, {"EXPIRE", "k2", "-9223372036854775808"}), ":1\r\n",
                "the most negative value still deletes");
    check_equal(run(store, {"GET", "k2"}), "$-1\r\n", "that key is gone too");
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
    test_nesting_depth_limit();
    test_serialization();
    test_round_trips();
    test_ping();
    test_set_and_get();
    test_del();
    test_case_insensitivity();
    test_command_errors();
    test_resp_to_command_integration();
    test_buffer_loop();
    test_expire_basics();
    test_ttl_meanings();
    test_key_actually_expires();
    test_ttl_decreases();
    test_set_clears_ttl();
    test_del_clears_ttl();
    test_expire_edge_values();
    test_expire_and_ttl_errors();
    test_expire_rejects_unrepresentable_deadlines();
    test_independent_expirations();
    test_ttl_over_resp();
    test_concurrent_pings();
    test_one_client_sets_another_gets();
    test_concurrent_independent_keys();
    test_concurrent_shared_key();
    test_concurrent_ttl();
    test_input_buffer_partial_reads();
    test_input_buffer_pipelining();
    test_input_buffer_mixed();
    test_output_buffer_partial_writes();
    test_client_state_is_per_fd();
    test_log_records_writes();
    test_log_records_expire();
    test_reads_are_not_logged();
    test_replay_rebuilds_the_store();
    test_replay_does_not_append();
    test_replay_ordering_and_ttl_rules();
    test_replay_drops_expired_keys();
    test_missing_and_empty_logs();
    test_malformed_log_is_refused();
    test_binary_safe_keys_and_values();
    test_lists();
    test_list_ranges();
    test_hashes();
    test_sets();
    test_wrong_type_errors();
    test_ttl_with_every_type();
    test_replacement_semantics();
    test_persistence_of_collections();
    test_persistence_of_collection_removals();
    test_persistence_emptied_collections();
    test_persistence_collection_ttl();
    test_persistence_replay_is_still_read_only();
    test_persistence_malformed_collection_records();
    test_persistence_awkward_collection_data();

    std::cout << "\n";
    if (failures == 0) {
        std::cout << "all tests passed\n";
        return 0;
    }

    std::cout << failures << " check(s) failed\n";
    return 1;
}
