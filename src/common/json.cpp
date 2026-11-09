#include "common/json.h"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace ad {
namespace {

constexpr int kMaxDepth = 32;

class Parser {
 public:
  explicit Parser(std::string_view text) : s_(text) {}

  Json parseDocument() {
    Json value = parseValue(0);
    skipSpace();
    if (pos_ != s_.size()) fail("trailing characters");
    return value;
  }

 private:
  [[noreturn]] void fail(const char* what) const {
    throw JsonError(std::string("json: ") + what + " at offset " + std::to_string(pos_));
  }
  void skipSpace() {
    while (pos_ < s_.size() &&
           (s_[pos_] == ' ' || s_[pos_] == '\n' || s_[pos_] == '\r' || s_[pos_] == '\t')) {
      ++pos_;
    }
  }
  char peek() const { return pos_ < s_.size() ? s_[pos_] : '\0'; }
  bool consume(char c) {
    if (peek() == c && pos_ < s_.size()) {
      ++pos_;
      return true;
    }
    return false;
  }
  void expectWord(std::string_view word) {
    if (s_.substr(pos_, word.size()) != word) fail("invalid literal");
    pos_ += word.size();
  }

  Json parseValue(int depth) {
    if (depth > kMaxDepth) fail("nesting too deep");
    skipSpace();
    switch (peek()) {
      case '{': return parseObject(depth);
      case '[': return parseArray(depth);
      case '"': return Json(parseString());
      case 't': expectWord("true"); return Json(true);
      case 'f': expectWord("false"); return Json(false);
      case 'n': expectWord("null"); return Json();
      default: return parseNumber();
    }
  }

  Json parseObject(int depth) {
    ++pos_;
    Json::Object members;
    skipSpace();
    if (consume('}')) return Json(std::move(members));
    for (;;) {
      skipSpace();
      if (peek() != '"') fail("expected object key");
      std::string key = parseString();
      skipSpace();
      if (!consume(':')) fail("expected ':'");
      Json value = parseValue(depth + 1);
      members.emplace_back(std::move(key), std::move(value));
      skipSpace();
      if (consume(',')) continue;
      if (consume('}')) break;
      fail("expected ',' or '}'");
    }
    return Json(std::move(members));
  }

  Json parseArray(int depth) {
    ++pos_;
    Json::Array items;
    skipSpace();
    if (consume(']')) return Json(std::move(items));
    for (;;) {
      items.push_back(parseValue(depth + 1));
      skipSpace();
      if (consume(',')) continue;
      if (consume(']')) break;
      fail("expected ',' or ']'");
    }
    return Json(std::move(items));
  }

  u32 parseHex4() {
    if (pos_ + 4 > s_.size()) fail("truncated \\u escape");
    u32 value = 0;
    const char* first = s_.data() + pos_;
    auto [end, ec] = std::from_chars(first, first + 4, value, 16);
    if (ec != std::errc{} || end != first + 4) fail("invalid \\u escape");
    pos_ += 4;
    return value;
  }

  static void appendUtf8(std::string& out, u32 cp) {
    if (cp < 0x80) {
      out += static_cast<char>(cp);
    } else if (cp < 0x800) {
      out += static_cast<char>(0xC0 | (cp >> 6));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out += static_cast<char>(0xE0 | (cp >> 12));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (cp >> 18));
      out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    }
  }

  std::string parseString() {
    ++pos_;
    std::string out;
    for (;;) {
      if (pos_ >= s_.size()) fail("unterminated string");
      const char c = s_[pos_++];
      if (c == '"') return out;
      if (static_cast<unsigned char>(c) < 0x20) fail("control character in string");
      if (c != '\\') {
        out += c;
        continue;
      }
      if (pos_ >= s_.size()) fail("unterminated escape");
      const char e = s_[pos_++];
      switch (e) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          u32 cp = parseHex4();
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (s_.substr(pos_, 2) != "\\u") fail("unpaired surrogate");
            pos_ += 2;
            const u32 low = parseHex4();
            if (low < 0xDC00 || low > 0xDFFF) fail("unpaired surrogate");
            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            fail("unpaired surrogate");
          }
          appendUtf8(out, cp);
          break;
        }
        default: fail("invalid escape");
      }
    }
  }

  static bool isDigit(char c) { return c >= '0' && c <= '9'; }

  Json parseNumber() {
    const std::size_t start = pos_;
    const bool negative = consume('-');
    if (!isDigit(peek())) fail("invalid value");
    if (peek() == '0' && pos_ + 1 < s_.size() && isDigit(s_[pos_ + 1])) fail("leading zero");
    while (isDigit(peek())) ++pos_;
    bool integral = true;
    if (peek() == '.') {
      integral = false;
      ++pos_;
      if (!isDigit(peek())) fail("invalid fraction");
      while (isDigit(peek())) ++pos_;
    }
    if (peek() == 'e' || peek() == 'E') {
      integral = false;
      ++pos_;
      if (peek() == '+' || peek() == '-') ++pos_;
      if (!isDigit(peek())) fail("invalid exponent");
      while (isDigit(peek())) ++pos_;
    }
    const char* first = s_.data() + start;
    const char* last = s_.data() + pos_;
    if (integral) {
      if (negative) {
        i64 v = 0;
        auto [end, ec] = std::from_chars(first, last, v);
        if (ec == std::errc{} && end == last) return Json(v);
      } else {
        u64 v = 0;
        auto [end, ec] = std::from_chars(first, last, v);
        if (ec == std::errc{} && end == last) return Json(v);
      }
    }
    const double d = std::strtod(std::string(first, last).c_str(), nullptr);
    if (!std::isfinite(d)) fail("number out of range");
    return Json(d);
  }

  std::string_view s_;
  std::size_t pos_ = 0;
};

void dumpString(const std::string& s, std::string& out) {
  out += '"';
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(c));
          out += buf;
        } else {
          out += c;
        }
    }
  }
  out += '"';
}

}

Json Json::parse(std::string_view text) { return Parser(text).parseDocument(); }

namespace {
struct Dumper {
  std::string& out;
  void operator()(std::monostate) const { out += "null"; }
  void operator()(bool v) const { out += v ? "true" : "false"; }
  void operator()(i64 v) const { out += std::to_string(v); }
  void operator()(u64 v) const { out += std::to_string(v); }
  void operator()(double v) const {
    if (!std::isfinite(v)) {
      out += "null";
      return;
    }
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.17g", v);
    out += buf;
  }
  void operator()(const std::string& v) const { dumpString(v, out); }
  void operator()(const Json::Array& v) const {
    out += '[';
    for (std::size_t i = 0; i < v.size(); ++i) {
      if (i != 0) out += ',';
      out += v[i].dump();
    }
    out += ']';
  }
  void operator()(const Json::Object& v) const {
    out += '{';
    for (std::size_t i = 0; i < v.size(); ++i) {
      if (i != 0) out += ',';
      dumpString(v[i].first, out);
      out += ':';
      out += v[i].second.dump();
    }
    out += '}';
  }
};
}

std::string Json::dump() const {
  std::string out;
  std::visit(Dumper{out}, v_);
  return out;
}

bool Json::asBool() const {
  if (const bool* v = std::get_if<bool>(&v_)) return *v;
  throw JsonError("json: expected boolean");
}

u64 Json::asU64() const {
  if (const u64* v = std::get_if<u64>(&v_)) return *v;
  if (const i64* v = std::get_if<i64>(&v_); v != nullptr && *v >= 0) return static_cast<u64>(*v);
  throw JsonError("json: expected non-negative integer");
}

i64 Json::asI64() const {
  if (const i64* v = std::get_if<i64>(&v_)) return *v;
  if (const u64* v = std::get_if<u64>(&v_);
      v != nullptr && *v <= static_cast<u64>(std::numeric_limits<i64>::max())) {
    return static_cast<i64>(*v);
  }
  throw JsonError("json: expected 64-bit integer");
}

double Json::asDouble() const {
  if (const double* v = std::get_if<double>(&v_)) return *v;
  if (const i64* v = std::get_if<i64>(&v_)) return static_cast<double>(*v);
  if (const u64* v = std::get_if<u64>(&v_)) return static_cast<double>(*v);
  throw JsonError("json: expected number");
}

const std::string& Json::asString() const {
  if (const std::string* v = std::get_if<std::string>(&v_)) return *v;
  throw JsonError("json: expected string");
}

const Json::Array& Json::asArray() const {
  if (const Array* v = std::get_if<Array>(&v_)) return *v;
  throw JsonError("json: expected array");
}

const Json::Object& Json::asObject() const {
  if (const Object* v = std::get_if<Object>(&v_)) return *v;
  throw JsonError("json: expected object");
}

const Json* Json::find(std::string_view key) const {
  const Object* obj = std::get_if<Object>(&v_);
  if (obj == nullptr) return nullptr;
  for (const Member& m : *obj) {
    if (m.first == key) return &m.second;
  }
  return nullptr;
}

const Json& Json::at(std::string_view key) const {
  const Json* v = find(key);
  if (v == nullptr) throw JsonError("json: missing field '" + std::string(key) + "'");
  return *v;
}

Json& Json::set(std::string key, Json value) {
  if (isNull()) v_ = Object{};
  std::get<Object>(v_).emplace_back(std::move(key), std::move(value));
  return *this;
}

}
