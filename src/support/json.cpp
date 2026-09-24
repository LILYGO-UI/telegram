#include "support/json.hpp"

#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>

namespace support::json {
namespace {

constexpr std::size_t kMaximumDepth = 512;

[[noreturn]] void throw_type_error(Type expected, Type actual)
{
    throw TypeError(expected, actual);
}

bool is_continuation_byte(unsigned char byte) noexcept
{
    return (byte & 0xc0U) == 0x80U;
}

std::size_t valid_utf8_sequence_length(std::string_view text, std::size_t offset)
{
    const auto first = static_cast<unsigned char>(text[offset]);
    if (first <= 0x7fU) return 1;

    if (first >= 0xc2U && first <= 0xdfU) {
        if (offset + 1 < text.size() && is_continuation_byte(static_cast<unsigned char>(text[offset + 1]))) return 2;
        return 0;
    }

    if (first >= 0xe0U && first <= 0xefU) {
        if (offset + 2 >= text.size()) return 0;
        const auto second = static_cast<unsigned char>(text[offset + 1]);
        const auto third  = static_cast<unsigned char>(text[offset + 2]);
        if (!is_continuation_byte(third)) return 0;
        if (first == 0xe0U) return second >= 0xa0U && second <= 0xbfU ? 3 : 0;
        if (first == 0xedU) return second >= 0x80U && second <= 0x9fU ? 3 : 0;
        return is_continuation_byte(second) ? 3 : 0;
    }

    if (first >= 0xf0U && first <= 0xf4U) {
        if (offset + 3 >= text.size()) return 0;
        const auto second = static_cast<unsigned char>(text[offset + 1]);
        const auto third  = static_cast<unsigned char>(text[offset + 2]);
        const auto fourth = static_cast<unsigned char>(text[offset + 3]);
        if (!is_continuation_byte(third) || !is_continuation_byte(fourth)) return 0;
        if (first == 0xf0U) return second >= 0x90U && second <= 0xbfU ? 4 : 0;
        if (first == 0xf4U) return second >= 0x80U && second <= 0x8fU ? 4 : 0;
        return is_continuation_byte(second) ? 4 : 0;
    }

    return 0;
}

void append_utf8(std::uint32_t code_point, std::string &output)
{
    if (code_point <= 0x7fU) {
        output.push_back(static_cast<char>(code_point));
    } else if (code_point <= 0x7ffU) {
        output.push_back(static_cast<char>(0xc0U | (code_point >> 6U)));
        output.push_back(static_cast<char>(0x80U | (code_point & 0x3fU)));
    } else if (code_point <= 0xffffU) {
        output.push_back(static_cast<char>(0xe0U | (code_point >> 12U)));
        output.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (code_point & 0x3fU)));
    } else {
        output.push_back(static_cast<char>(0xf0U | (code_point >> 18U)));
        output.push_back(static_cast<char>(0x80U | ((code_point >> 12U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (code_point & 0x3fU)));
    }
}

void append_escaped_string(std::string_view value, std::string &output)
{
    static constexpr char hex[] = "0123456789abcdef";

    output.push_back('"');
    for (std::size_t offset = 0; offset < value.size();) {
        const auto byte = static_cast<unsigned char>(value[offset]);
        switch (byte) {
            case '"':
                output += "\\\"";
                ++offset;
                break;
            case '\\':
                output += "\\\\";
                ++offset;
                break;
            case '\b':
                output += "\\b";
                ++offset;
                break;
            case '\f':
                output += "\\f";
                ++offset;
                break;
            case '\n':
                output += "\\n";
                ++offset;
                break;
            case '\r':
                output += "\\r";
                ++offset;
                break;
            case '\t':
                output += "\\t";
                ++offset;
                break;
            default:
                if (byte < 0x20U) {
                    output += "\\u00";
                    output.push_back(hex[(byte >> 4U) & 0x0fU]);
                    output.push_back(hex[byte & 0x0fU]);
                    ++offset;
                } else if (byte < 0x80U) {
                    output.push_back(static_cast<char>(byte));
                    ++offset;
                } else {
                    const auto length = valid_utf8_sequence_length(value, offset);
                    if (length == 0) throw std::invalid_argument("cannot stringify a string containing invalid UTF-8");
                    output.append(value, offset, length);
                    offset += length;
                }
                break;
        }
    }
    output.push_back('"');
}

void append_value(const Value &value, std::string &output, std::size_t depth)
{
    if (depth > kMaximumDepth) throw std::length_error("JSON value exceeds the maximum nesting depth");

    switch (value.type()) {
        case Type::null:
            output += "null";
            return;
        case Type::boolean:
            output += value.as_bool() ? "true" : "false";
            return;
        case Type::number:
            output += value.number_text();
            return;
        case Type::string:
            append_escaped_string(value.as_string(), output);
            return;
        case Type::array: {
            output.push_back('[');
            bool first = true;
            for (const auto &item : value.as_array()) {
                if (!first) output.push_back(',');
                first = false;
                append_value(item, output, depth + 1);
            }
            output.push_back(']');
            return;
        }
        case Type::object: {
            output.push_back('{');
            bool first = true;
            for (const auto &[key, item] : value.as_object()) {
                if (!first) output.push_back(',');
                first = false;
                append_escaped_string(key, output);
                output.push_back(':');
                append_value(item, output, depth + 1);
            }
            output.push_back('}');
            return;
        }
    }
}

}  // namespace

namespace detail {

class Parser {
public:
    explicit Parser(std::string_view input) : input_(input)
    {
    }

    Value parse_document()
    {
        skip_whitespace();
        Value result = parse_value(0);
        skip_whitespace();
        if (position_ != input_.size()) fail("unexpected trailing data");
        return result;
    }

private:
    [[noreturn]] void fail(const std::string &message, std::size_t offset) const
    {
        throw ParseError(offset, message);
    }

    [[noreturn]] void fail(const std::string &message) const
    {
        fail(message, position_);
    }

    void skip_whitespace() noexcept
    {
        while (position_ < input_.size()) {
            const char current = input_[position_];
            if (current != ' ' && current != '\t' && current != '\n' && current != '\r') return;
            ++position_;
        }
    }

    Value parse_value(std::size_t depth)
    {
        if (depth > kMaximumDepth) fail("maximum nesting depth exceeded");
        if (position_ == input_.size()) fail("expected a JSON value");

        switch (input_[position_]) {
            case 'n':
                consume_literal("null");
                return nullptr;
            case 't':
                consume_literal("true");
                return true;
            case 'f':
                consume_literal("false");
                return false;
            case '"':
                return parse_string();
            case '[':
                return parse_array(depth);
            case '{':
                return parse_object(depth);
            default:
                if (input_[position_] == '-' || (input_[position_] >= '0' && input_[position_] <= '9')) {
                    return parse_number();
                }
                fail("expected a JSON value");
        }
    }

    void consume_literal(std::string_view literal)
    {
        if (input_.substr(position_, literal.size()) != literal) fail("invalid literal");
        position_ += literal.size();
    }

    Value parse_array(std::size_t depth)
    {
        ++position_;
        skip_whitespace();
        Value::Array result;
        if (consume_if(']')) return result;

        while (true) {
            result.push_back(parse_value(depth + 1));
            skip_whitespace();
            if (consume_if(']')) return result;
            if (!consume_if(',')) fail("expected ',' or ']' in array");
            skip_whitespace();
        }
    }

    Value parse_object(std::size_t depth)
    {
        ++position_;
        skip_whitespace();
        Value::Object result;
        if (consume_if('}')) return result;

        while (true) {
            if (position_ == input_.size() || input_[position_] != '"') fail("expected a string key in object");
            const std::size_t key_offset = position_;
            std::string key              = parse_string();
            skip_whitespace();
            if (!consume_if(':')) fail("expected ':' after object key");
            skip_whitespace();
            Value item = parse_value(depth + 1);
            if (!result.emplace(std::move(key), std::move(item)).second) fail("duplicate object key", key_offset);
            skip_whitespace();
            if (consume_if('}')) return result;
            if (!consume_if(',')) fail("expected ',' or '}' in object");
            skip_whitespace();
        }
    }

    std::string parse_string()
    {
        ++position_;
        std::string result;
        while (position_ < input_.size()) {
            const auto byte = static_cast<unsigned char>(input_[position_]);
            if (byte == '"') {
                ++position_;
                return result;
            }
            if (byte == '\\') {
                parse_escape(result);
                continue;
            }
            if (byte < 0x20U) fail("unescaped control character in string");
            if (byte < 0x80U) {
                result.push_back(static_cast<char>(byte));
                ++position_;
                continue;
            }

            const auto length = valid_utf8_sequence_length(input_, position_);
            if (length == 0) fail("invalid UTF-8 in string");
            result.append(input_, position_, length);
            position_ += length;
        }
        fail("unterminated string");
    }

    void parse_escape(std::string &result)
    {
        ++position_;
        if (position_ == input_.size()) fail("unterminated escape sequence");
        const char escaped = input_[position_++];
        switch (escaped) {
            case '"':
            case '\\':
            case '/':
                result.push_back(escaped);
                return;
            case 'b':
                result.push_back('\b');
                return;
            case 'f':
                result.push_back('\f');
                return;
            case 'n':
                result.push_back('\n');
                return;
            case 'r':
                result.push_back('\r');
                return;
            case 't':
                result.push_back('\t');
                return;
            case 'u':
                parse_unicode_escape(result);
                return;
            default:
                fail("invalid escape sequence", position_ - 1);
        }
    }

    std::uint16_t parse_hex_quad()
    {
        if (input_.size() - position_ < 4) fail("incomplete Unicode escape");
        std::uint16_t value = 0;
        for (int index = 0; index < 4; ++index) {
            const char digit = input_[position_++];
            value            = static_cast<std::uint16_t>(value << 4U);
            if (digit >= '0' && digit <= '9') {
                value = static_cast<std::uint16_t>(value | static_cast<unsigned>(digit - '0'));
            } else if (digit >= 'a' && digit <= 'f') {
                value = static_cast<std::uint16_t>(value | static_cast<unsigned>(digit - 'a' + 10));
            } else if (digit >= 'A' && digit <= 'F') {
                value = static_cast<std::uint16_t>(value | static_cast<unsigned>(digit - 'A' + 10));
            } else {
                fail("invalid hexadecimal digit in Unicode escape", position_ - 1);
            }
        }
        return value;
    }

    void parse_unicode_escape(std::string &result)
    {
        const auto first = parse_hex_quad();
        if (first >= 0xd800U && first <= 0xdbffU) {
            if (input_.size() - position_ < 2 || input_[position_] != '\\' || input_[position_ + 1] != 'u') {
                fail("high surrogate must be followed by a low surrogate");
            }
            position_ += 2;
            const auto second = parse_hex_quad();
            if (second < 0xdc00U || second > 0xdfffU) fail("invalid low surrogate");
            const std::uint32_t code_point =
                0x10000U + ((static_cast<std::uint32_t>(first) - 0xd800U) << 10U) + (second - 0xdc00U);
            append_utf8(code_point, result);
            return;
        }
        if (first >= 0xdc00U && first <= 0xdfffU) fail("unexpected low surrogate");
        append_utf8(first, result);
    }

    Value parse_number()
    {
        const std::size_t start = position_;
        consume_if('-');
        if (position_ == input_.size()) fail("incomplete number");

        if (input_[position_] == '0') {
            ++position_;
            if (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') {
                fail("leading zero in number");
            }
        } else {
            if (input_[position_] < '1' || input_[position_] > '9') fail("expected digit in number");
            while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') ++position_;
        }

        if (consume_if('.')) {
            if (position_ == input_.size() || input_[position_] < '0' || input_[position_] > '9') {
                fail("expected digit after decimal point");
            }
            while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') ++position_;
        }

        if (position_ < input_.size() && (input_[position_] == 'e' || input_[position_] == 'E')) {
            ++position_;
            if (position_ < input_.size() && (input_[position_] == '+' || input_[position_] == '-')) ++position_;
            if (position_ == input_.size() || input_[position_] < '0' || input_[position_] > '9') {
                fail("expected digit in exponent");
            }
            while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') ++position_;
        }

        return Value(Value::Number{std::string(input_.substr(start, position_ - start))});
    }

    bool consume_if(char expected) noexcept
    {
        if (position_ == input_.size() || input_[position_] != expected) return false;
        ++position_;
        return true;
    }

    std::string_view input_;
    std::size_t position_ = 0;
};

}  // namespace detail

const char *type_name(Type type) noexcept
{
    switch (type) {
        case Type::null:
            return "null";
        case Type::boolean:
            return "boolean";
        case Type::number:
            return "number";
        case Type::string:
            return "string";
        case Type::array:
            return "array";
        case Type::object:
            return "object";
    }
    return "unknown";
}

ParseError::ParseError(std::size_t offset, const std::string &message)
    : std::runtime_error("JSON parse error at byte " + std::to_string(offset) + ": " + message), offset_(offset)
{
}

std::size_t ParseError::offset() const noexcept
{
    return offset_;
}

TypeError::TypeError(Type expected, Type actual)
    : std::logic_error(std::string("expected JSON ") + type_name(expected) + ", got " + type_name(actual))
{
}

Value::Value() noexcept : storage_(nullptr)
{
}

Value::Value(std::nullptr_t) noexcept : storage_(nullptr)
{
}

Value::Value(bool value) noexcept : storage_(value)
{
}

Value::Value(double value)
{
    if (!std::isfinite(value)) throw std::invalid_argument("JSON numbers must be finite");
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
    storage_ = Number{stream.str()};
}

Value::Value(const char *value) : storage_(std::string(value ? value : ""))
{
    if (!value) throw std::invalid_argument("JSON string cannot be constructed from a null pointer");
}

Value::Value(std::string value) : storage_(std::move(value))
{
}

Value::Value(Array value) : storage_(std::move(value))
{
}

Value::Value(Object value) : storage_(std::move(value))
{
}

Value::Value(Number number) : storage_(std::move(number))
{
}

Type Value::type() const noexcept
{
    return static_cast<Type>(storage_.index());
}

bool Value::is_null() const noexcept
{
    return std::holds_alternative<std::nullptr_t>(storage_);
}

bool Value::is_bool() const noexcept
{
    return std::holds_alternative<bool>(storage_);
}

bool Value::is_number() const noexcept
{
    return std::holds_alternative<Number>(storage_);
}

bool Value::is_string() const noexcept
{
    return std::holds_alternative<std::string>(storage_);
}

bool Value::is_array() const noexcept
{
    return std::holds_alternative<Array>(storage_);
}

bool Value::is_object() const noexcept
{
    return std::holds_alternative<Object>(storage_);
}

bool Value::as_bool() const
{
    if (!is_bool()) throw_type_error(Type::boolean, type());
    return std::get<bool>(storage_);
}

double Value::as_double() const
{
    if (!is_number()) throw_type_error(Type::number, type());
    std::istringstream stream(number_text());
    stream.imbue(std::locale::classic());
    double result = 0.0;
    stream >> result;
    if (!stream || !stream.eof() || !std::isfinite(result))
        throw std::out_of_range("JSON number is outside double range");
    return result;
}

std::int64_t Value::as_int64() const
{
    if (!is_number()) throw_type_error(Type::number, type());
    const auto &text = number_text();
    if (text.find_first_of(".eE") != std::string::npos) throw std::invalid_argument("JSON number is not an integer");
    std::int64_t result = 0;
    const auto parsed   = std::from_chars(text.data(), text.data() + text.size(), result);
    if (parsed.ec == std::errc::result_out_of_range) throw std::out_of_range("JSON integer is outside int64 range");
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        throw std::invalid_argument("JSON number is not an integer");
    }
    return result;
}

std::uint64_t Value::as_uint64() const
{
    if (!is_number()) throw_type_error(Type::number, type());
    const auto &text = number_text();
    if (text.find_first_of("-.eE") != std::string::npos) {
        throw std::invalid_argument("JSON number is not a non-negative integer");
    }
    std::uint64_t result = 0;
    const auto parsed    = std::from_chars(text.data(), text.data() + text.size(), result);
    if (parsed.ec == std::errc::result_out_of_range) throw std::out_of_range("JSON integer is outside uint64 range");
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        throw std::invalid_argument("JSON number is not a non-negative integer");
    }
    return result;
}

const std::string &Value::number_text() const
{
    if (!is_number()) throw_type_error(Type::number, type());
    return std::get<Number>(storage_).text;
}

const std::string &Value::as_string() const
{
    if (!is_string()) throw_type_error(Type::string, type());
    return std::get<std::string>(storage_);
}

std::string &Value::as_string()
{
    if (!is_string()) throw_type_error(Type::string, type());
    return std::get<std::string>(storage_);
}

const Value::Array &Value::as_array() const
{
    if (!is_array()) throw_type_error(Type::array, type());
    return std::get<Array>(storage_);
}

Value::Array &Value::as_array()
{
    if (!is_array()) throw_type_error(Type::array, type());
    return std::get<Array>(storage_);
}

const Value::Object &Value::as_object() const
{
    if (!is_object()) throw_type_error(Type::object, type());
    return std::get<Object>(storage_);
}

Value::Object &Value::as_object()
{
    if (!is_object()) throw_type_error(Type::object, type());
    return std::get<Object>(storage_);
}

const Value &Value::at(std::size_t index) const
{
    return as_array().at(index);
}

Value &Value::at(std::size_t index)
{
    return as_array().at(index);
}

const Value &Value::at(std::string_view key) const
{
    const auto &object = as_object();
    const auto found   = object.find(key);
    if (found == object.end()) throw std::out_of_range("JSON object key not found: " + std::string(key));
    return found->second;
}

Value &Value::at(std::string_view key)
{
    auto &object     = as_object();
    const auto found = object.find(key);
    if (found == object.end()) throw std::out_of_range("JSON object key not found: " + std::string(key));
    return found->second;
}

const Value *Value::find(std::string_view key) const
{
    const auto &object = as_object();
    const auto found   = object.find(key);
    return found == object.end() ? nullptr : &found->second;
}

Value *Value::find(std::string_view key)
{
    auto &object     = as_object();
    const auto found = object.find(key);
    return found == object.end() ? nullptr : &found->second;
}

bool Value::contains(std::string_view key) const
{
    return find(key) != nullptr;
}

Value parse(std::string_view input)
{
    return detail::Parser(input).parse_document();
}

std::string stringify(const Value &value)
{
    std::string output;
    append_value(value, output, 0);
    return output;
}

}  // namespace support::json
