#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace ad {

std::string percentEncode(std::string_view input);

// Returns nullopt on a malformed escape.
std::optional<std::string> percentDecode(std::string_view input);

// Decodes "a=1&b=x%20y". Pairs that fail to decode are dropped.
std::unordered_map<std::string, std::string> parseQuery(std::string_view query);

}
