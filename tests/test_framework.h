#pragma once

#include <chrono>
#include <cmath>
#include <concepts>
#include <functional>
#include <ostream>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace adtest {

struct TestCase {
  const char* name;
  void (*body)();
};

std::vector<TestCase>& registry();

struct Registrar {
  Registrar(const char* name, void (*body)()) { registry().push_back({name, body}); }
};

struct TestAbort {};

// Safe to call from any thread.
void recordFailure(const char* file, int line, const std::string& message);

template <class T>
concept Streamable = requires(std::ostream& os, const T& value) { os << value; };

template <class T>
std::string show(const T& value) {
  if constexpr (Streamable<T>) {
    std::ostringstream out;
    out << value;
    return out.str();
  } else {
    return "<unprintable>";
  }
}

// The integer types std::cmp_equal accepts: no bool and no character types.
template <class T>
concept PlainInt = std::integral<T> && !std::same_as<T, bool> && !std::same_as<T, char> &&
                   !std::same_as<T, wchar_t> && !std::same_as<T, char8_t> &&
                   !std::same_as<T, char16_t> && !std::same_as<T, char32_t>;

template <class A, class B>
bool equalTo(const A& a, const B& b) {
  if constexpr (PlainInt<A> && PlainInt<B>) {
    return std::cmp_equal(a, b);
  } else {
    return a == b;
  }
}

template <class A, class B>
bool lessThan(const A& a, const B& b) {
  if constexpr (PlainInt<A> && PlainInt<B>) {
    return std::cmp_less(a, b);
  } else {
    return a < b;
  }
}

// The operands are taken as arguments so that temporaries inside them, such as
// the optional in make().value(), live until the comparison is finished.
template <class A, class B, class Predicate>
void checkBinary(const char* file, int line, bool fatal, const char* text, const A& a, const B& b,
                 Predicate predicate) {
  if (predicate(a, b)) return;
  recordFailure(file, line, std::string(text) + " failed: " + show(a) + " vs " + show(b));
  if (fatal) throw TestAbort{};
}

inline bool waitUntil(const std::function<bool()>& predicate, int timeoutMs = 5000) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return predicate();
}

}

#define TEST(name)                                                  \
  static void adtest_body_##name();                                 \
  static ::adtest::Registrar adtest_reg_##name(#name, adtest_body_##name); \
  static void adtest_body_##name()

#define AD_CHECK_(fatal, passed, message)                        \
  do {                                                           \
    if (!(passed)) {                                             \
      ::adtest::recordFailure(__FILE__, __LINE__, (message));    \
      if (fatal) throw ::adtest::TestAbort{};                    \
    }                                                            \
  } while (false)

#define EXPECT_TRUE(c) AD_CHECK_(false, static_cast<bool>(c), std::string("expected true: " #c))
#define EXPECT_FALSE(c) AD_CHECK_(false, !static_cast<bool>(c), std::string("expected false: " #c))
#define ASSERT_TRUE(c) AD_CHECK_(true, static_cast<bool>(c), std::string("required true: " #c))
#define ASSERT_FALSE(c) AD_CHECK_(true, !static_cast<bool>(c), std::string("required false: " #c))

#define AD_BINARY_(fatal, a, b, op, expression)                                    ::adtest::checkBinary(__FILE__, __LINE__, fatal, #a " " op " " #b, (a), (b),                           [&](const auto& x, const auto& y) { return expression; })

#define EXPECT_EQ(a, b) AD_BINARY_(false, a, b, "==", ::adtest::equalTo(x, y))
#define EXPECT_NE(a, b) AD_BINARY_(false, a, b, "!=", !::adtest::equalTo(x, y))
#define EXPECT_LT(a, b) AD_BINARY_(false, a, b, "<", ::adtest::lessThan(x, y))
#define EXPECT_LE(a, b) AD_BINARY_(false, a, b, "<=", !::adtest::lessThan(y, x))
#define EXPECT_GT(a, b) AD_BINARY_(false, a, b, ">", ::adtest::lessThan(y, x))
#define EXPECT_GE(a, b) AD_BINARY_(false, a, b, ">=", !::adtest::lessThan(x, y))
#define ASSERT_EQ(a, b) AD_BINARY_(true, a, b, "==", ::adtest::equalTo(x, y))

#define EXPECT_NEAR(a, b, tolerance)                                        AD_BINARY_(false, a, b, "~=",                                                        std::fabs(static_cast<double>(x) - static_cast<double>(y)) <= static_cast<double>(tolerance))

#define EXPECT_THROW(expression, ExceptionType)                                     \
  do {                                                                              \
    bool ad_caught = false;                                                         \
    try {                                                                           \
      (void)(expression);                                                           \
    } catch (const ExceptionType&) {                                                \
      ad_caught = true;                                                             \
    } catch (...) {                                                                 \
    }                                                                               \
    AD_CHECK_(false, ad_caught, std::string("expected " #ExceptionType " from: " #expression)); \
  } while (false)
