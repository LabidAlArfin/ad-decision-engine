#include "common/url.h"

namespace ad {
namespace {

int hexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}

std::string percentEncode(std::string_view input) {
  static constexpr char kDigits[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(input.size());
  for (char ch : input) {
    const auto c = static_cast<unsigned char>(ch);
    const bool unreserved = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
    if (unreserved) {
      out += ch;
    } else {
      out += '%';
      out += kDigits[c >> 4];
      out += kDigits[c & 0xF];
    }
  }
  return out;
}

std::optional<std::string> percentDecode(std::string_view input) {
  std::string out;
  out.reserve(input.size());
  for (std::size_t i = 0; i < input.size(); ++i) {
    if (input[i] == '+') {
      out += ' ';
    } else if (input[i] == '%') {
      if (i + 2 >= input.size()) return std::nullopt;
      const int hi = hexValue(input[i + 1]);
      const int lo = hexValue(input[i + 2]);
      if (hi < 0 || lo < 0) return std::nullopt;
      out += static_cast<char>(hi * 16 + lo);
      i += 2;
    } else {
      out += input[i];
    }
  }
  return out;
}

std::unordered_map<std::string, std::string> parseQuery(std::string_view query) {
  std::unordered_map<std::string, std::string> out;
  while (!query.empty()) {
    const std::size_t amp = query.find('&');
    const std::string_view pair = query.substr(0, amp);
    query = amp == std::string_view::npos ? std::string_view{} : query.substr(amp + 1);
    const std::size_t eq = pair.find('=');
    if (eq == std::string_view::npos) continue;
    auto key = percentDecode(pair.substr(0, eq));
    auto value = percentDecode(pair.substr(eq + 1));
    if (key && value) out.insert_or_assign(std::move(*key), std::move(*value));
  }
  return out;
}

}
