#ifndef LILYGO_UI_TELEGRAM_SUPPORT_JSON_HPP
#define LILYGO_UI_TELEGRAM_SUPPORT_JSON_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace support::json {

namespace detail {
class Parser;
}

enum class Type {
    null,
    boolean,
    number,
    string,
    array,
    object,
};

const char *type_name(Type type) noexcept;

class ParseError : public std::runtime_error {
public:
    ParseError(std::size_t offset, const std::string &message);

    [[nodiscard]] std::size_t offset() const noexcept;

private:
    std::size_t offset_;
};

class TypeError : public std::logic_error {
public:
    TypeError(Type expected, Type actual);
};

class Value {
private:
    struct Number {
        std::string text;

        bool operator==(const Number &other) const noexcept
        {
            return text == other.text;
        }
    };

public:
    using Array  = std::vector<Value>;
    using Object = std::map<std::string, Value, std::less<>>;

    Value() noexcept;
    Value(std::nullptr_t) noexcept;
    Value(bool value) noexcept;

    template <
        typename Integer,
        std::enable_if_t<std::is_integral_v<Integer> && !std::is_same_v<std::remove_cv_t<Integer>, bool>, int> = 0>
    Value(Integer value) : storage_(Number{integer_to_string(value)})
    {
    }

    Value(double value);
    Value(const char *value);
    Value(std::string value);
    Value(Array value);
    Value(Object value);

    [[nodiscard]] Type type() const noexcept;
    [[nodiscard]] bool is_null() const noexcept;
    [[nodiscard]] bool is_bool() const noexcept;
    [[nodiscard]] bool is_number() const noexcept;
    [[nodiscard]] bool is_string() const noexcept;
    [[nodiscard]] bool is_array() const noexcept;
    [[nodiscard]] bool is_object() const noexcept;

    [[nodiscard]] bool as_bool() const;
    [[nodiscard]] double as_double() const;
    [[nodiscard]] std::int64_t as_int64() const;
    [[nodiscard]] std::uint64_t as_uint64() const;
    [[nodiscard]] const std::string &number_text() const;
    [[nodiscard]] const std::string &as_string() const;
    [[nodiscard]] std::string &as_string();
    [[nodiscard]] const Array &as_array() const;
    [[nodiscard]] Array &as_array();
    [[nodiscard]] const Object &as_object() const;
    [[nodiscard]] Object &as_object();

    [[nodiscard]] const Value &at(std::size_t index) const;
    [[nodiscard]] Value &at(std::size_t index);
    [[nodiscard]] const Value &at(std::string_view key) const;
    [[nodiscard]] Value &at(std::string_view key);
    [[nodiscard]] const Value *find(std::string_view key) const;
    [[nodiscard]] Value *find(std::string_view key);
    [[nodiscard]] bool contains(std::string_view key) const;

    friend bool operator==(const Value &left, const Value &right) noexcept
    {
        return left.storage_ == right.storage_;
    }

    friend bool operator!=(const Value &left, const Value &right) noexcept
    {
        return !(left == right);
    }

private:
    using Storage = std::variant<std::nullptr_t, bool, Number, std::string, Array, Object>;

    explicit Value(Number number);

    template <typename Integer>
    static std::string integer_to_string(Integer value)
    {
        if constexpr (std::is_signed_v<Integer>) {
            return std::to_string(static_cast<long long>(value));
        } else {
            return std::to_string(static_cast<unsigned long long>(value));
        }
    }

    Storage storage_;

    friend class detail::Parser;
};

Value parse(std::string_view input);
std::string stringify(const Value &value);

}  // namespace support::json

#endif
