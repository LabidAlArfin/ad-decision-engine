#pragma once

#include <array>
#include <string>
#include <string_view>

#include "common/types.h"

namespace ad {

using Sha256Digest = std::array<u8, 32>;

Sha256Digest sha256(std::string_view data);
Sha256Digest hmacSha256(std::string_view key, std::string_view message);
std::string toHex(const Sha256Digest& digest);

bool constantTimeEquals(std::string_view a, std::string_view b);

std::string randomHex(std::size_t bytes);

// Signs and checks the query string of tracking URLs, so that a client cannot
// alter the price, campaign or redirect target carried in the link.
class UrlSigner {
 public:
  explicit UrlSigner(std::string secret) : secret_(std::move(secret)) {}

  // Returns "<path>?<query>&sig=<hex>". The path is covered by the signature,
  // so a signed click link cannot be replayed as an impression.
  std::string sign(std::string_view path, std::string_view query) const;

  // Takes the raw request target exactly as it was received.
  bool verify(std::string_view target) const;

 private:
  std::string secret_;
};

}
