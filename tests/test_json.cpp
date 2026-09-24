#include "support/json.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

template <typename Exception, typename Function>
void expect_throws(Function &&function)
{
    bool threw = false;
    try {
        function();
    } catch (const Exception &) {
        threw = true;
    }
    assert(threw);
}

void test_parse_document()
{
    using support::json::Value;

    const auto value = support::json::parse(
        R"({"array":[null,true,false,-12,3.5,6.02e23],"escaped":"line\n\t\\\"\/","name":"telegram"})");
    assert(value.is_object());
    assert(value.at("name").as_string() == "telegram");
    assert(value.at("escaped").as_string() == "line\n\t\\\"/");

    const Value::Array &array = value.at("array").as_array();
    assert(array.size() == 6);
    assert(array[0].is_null());
    assert(array[1].as_bool());
    assert(!array[2].as_bool());
    assert(array[3].as_int64() == -12);
    assert(std::abs(array[4].as_double() - 3.5) < 0.0001);
    assert(array[5].number_text() == "6.02e23");
}

void test_unicode()
{
    const auto value           = support::json::parse(R"("Latin \u0041, CJK \u4e2d, emoji \uD83D\uDE00")");
    const std::string expected = "Latin A, CJK \xe4\xb8\xad, emoji \xf0\x9f\x98\x80";
    assert(value.as_string() == expected);
    assert(support::json::parse(support::json::stringify(value)) == value);
}

void test_exact_integers()
{
    const auto signed_value = support::json::parse("-9223372036854775808");
    assert(signed_value.as_int64() == std::numeric_limits<std::int64_t>::min());
    assert(support::json::stringify(signed_value) == "-9223372036854775808");

    const auto unsigned_value = support::json::parse("18446744073709551615");
    assert(unsigned_value.as_uint64() == std::numeric_limits<std::uint64_t>::max());
    assert(support::json::stringify(unsigned_value) == "18446744073709551615");
    expect_throws<std::out_of_range>([&] { static_cast<void>(unsigned_value.as_int64()); });
    expect_throws<std::invalid_argument>([&] { static_cast<void>(support::json::parse("1.0").as_int64()); });
}

void test_dom_and_stringify()
{
    using support::json::Value;

    Value::Object object;
    object.emplace("message", Value("quote: \"; newline:\n"));
    object.emplace("ok", Value(true));
    object.emplace("request_id", Value(std::uint64_t{9007199254740993ULL}));
    object.emplace("values", Value(Value::Array{Value(nullptr), Value(-2), Value(0.25)}));
    const Value value(std::move(object));

    assert(value.contains("ok"));
    assert(!value.contains("missing"));
    assert(value.find("missing") == nullptr);
    assert(value.at("values").at(1).as_int64() == -2);

    const std::string encoded = support::json::stringify(value);
    assert(encoded ==
           R"({"message":"quote: \"; newline:\n","ok":true,"request_id":9007199254740993,"values":[null,-2,0.25]})");
    assert(support::json::parse(encoded) == value);
}

void test_type_errors()
{
    using support::json::TypeError;
    using support::json::Value;

    const Value string("text");
    expect_throws<TypeError>([&] { static_cast<void>(string.as_bool()); });
    expect_throws<TypeError>([&] { static_cast<void>(string.as_array()); });
    expect_throws<TypeError>([&] { static_cast<void>(string.find("key")); });
    expect_throws<std::out_of_range>([&] { static_cast<void>(Value(Value::Object{}).at("missing")); });
    expect_throws<std::invalid_argument>([] { static_cast<void>(Value(static_cast<const char *>(nullptr))); });
    expect_throws<std::invalid_argument>([] { static_cast<void>(Value(std::numeric_limits<double>::infinity())); });
}

void test_invalid_documents()
{
    using support::json::ParseError;

    constexpr std::string_view invalid_documents[] = {
        "",
        " ",
        "nul",
        "True",
        "01",
        "-",
        "1.",
        "1e",
        "[1,]",
        "{\"x\":1,}",
        "{\"x\":1,\"x\":2}",
        "\"\\x\"",
        "\"\\u12\"",
        "\"\\uD800\"",
        "\"\\uDC00\"",
        "true false",
    };
    for (const auto document : invalid_documents) {
        expect_throws<ParseError>([&] { static_cast<void>(support::json::parse(document)); });
    }

    const std::string invalid_utf8_json{"\"\xc0\x80\"", 4};
    expect_throws<ParseError>([&] { static_cast<void>(support::json::parse(invalid_utf8_json)); });

    const std::string invalid_utf8_value{"\xc0\x80", 2};
    expect_throws<std::invalid_argument>(
        [&] { static_cast<void>(support::json::stringify(support::json::Value(invalid_utf8_value))); });
}

}  // namespace

int main()
{
    test_parse_document();
    test_unicode();
    test_exact_integers();
    test_dom_and_stringify();
    test_type_errors();
    test_invalid_documents();
    return 0;
}
