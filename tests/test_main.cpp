#include <exception>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>

#include "test_framework.h"

namespace adtest {
namespace {

std::mutex gMutex;
int gFailures = 0;

}

std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

void recordFailure(const char* file, int line, const std::string& message) {
  std::lock_guard lock(gMutex);
  ++gFailures;
  std::cout << "  " << file << ":" << line << ": " << message << "\n" << std::flush;
}

}

// Usage: ad_engine_tests [substring]. With a substring, runs only matching tests.
int main(int argc, char** argv) {
  const std::string_view filter = argc > 1 ? argv[1] : "";
  int ran = 0;
  int failed = 0;

  for (const adtest::TestCase& test : adtest::registry()) {
    if (!filter.empty() && std::string_view(test.name).find(filter) == std::string_view::npos) continue;
    ++ran;
    {
      std::lock_guard lock(adtest::gMutex);
      adtest::gFailures = 0;
    }
    try {
      test.body();
    } catch (const adtest::TestAbort&) {
    } catch (const std::exception& e) {
      adtest::recordFailure(test.name, 0, std::string("unexpected exception: ") + e.what());
    } catch (...) {
      adtest::recordFailure(test.name, 0, "unexpected exception of unknown type");
    }
    std::lock_guard lock(adtest::gMutex);
    if (adtest::gFailures != 0) {
      ++failed;
      std::cout << "FAILED " << test.name << "\n" << std::flush;
    }
  }

  std::cout << (ran - failed) << "/" << ran << " tests passed\n";
  if (ran == 0) {
    std::cout << "no tests matched\n";
    return 1;
  }
  return failed == 0 ? 0 : 1;
}
