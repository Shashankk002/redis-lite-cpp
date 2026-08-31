#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "commands.hpp"
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
    test_independent_expirations();
    test_ttl_over_resp();

    std::cout << "\n";
    if (failures == 0) {
        std::cout << "all tests passed\n";
        return 0;
    }

    std::cout << failures << " check(s) failed\n";
    return 1;
}
