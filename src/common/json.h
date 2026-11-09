#pragma once

#include <concepts>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "common/types.h"

namespace ad {

class JsonError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// Minimal JSON value. Integers keep full 64-bit precision, which matters for
// request and user ids. parse() takes untrusted input: nesting is capped.
class Json {
 public:
  using Array = std::vector<Json>;
  using Member = std::pair<std::string, Json>;
  using Object = std::vector<Member>;

  Json() = default;
  Json(std::nullptr_t) {}
  Json(bool v) : v_(v) {}
  template <std::integral T>
    requires(!std::same_as<T, bool>)
  Json(T v) {
    if constexpr (std::signed_integral<T>) {
      v_ = static_cast<i64>(v);
    } else {
      v_ = static_cast<u64>(v);
    }
  }
  Json(double v) : v_(v) {}
  Json(const char* v) : v_(std::string(v)) {}
  Json(std::string v) : v_(std::move(v)) {}
  Json(std::string_view v) : v_(std::string(v)) {}
  Json(Array v) : v_(std::move(v)) {}
  Json(Object v) : v_(std::move(v)) {}

  static Json parse(std::string_view text);
  std::string dump() const;

  bool isNull() const { return std::holds_alternative<std::monostate>(v_); }
  bool isString() const { return std::holds_alternative<std::string>(v_); }
  bool isArray() const { return std::holds_alternative<Array>(v_); }
  bool isObject() const { return std::holds_alternative<Object>(v_); }

  bool asBool() const;
  u64 asU64() const;
  i64 asI64() const;
  double asDouble() const;
  const std::string& asString() const;
  const Array& asArray() const;
  const Object& asObject() const;

  // Object lookup. find() returns nullptr when absent; at() throws JsonError.
  const Json* find(std::string_view key) const;
  const Json& at(std::string_view key) const;
  Json& set(std::string key, Json value);

 private:
  std::variant<std::monostate, bool, i64, u64, double, std::string, Array, Object> v_;
};

}
