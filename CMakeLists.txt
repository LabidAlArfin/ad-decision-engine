cmake_minimum_required(VERSION 3.20)
project(ad_decision_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(Threads REQUIRED)

# An OBJECT library, so that no archiver is needed. Zig's toolchain on Windows
# has none that CMake can find on its own.
add_library(ad_core OBJECT
  src/app/app.cpp
  src/catalog/catalog.cpp
  src/common/crypto.cpp
  src/common/json.cpp
  src/common/net.cpp
  src/common/roaring.cpp
  src/common/url.cpp
  src/core/decision_engine.cpp
  src/dev/mini_redis.cpp
  src/events/events.cpp
  src/observability/observability.cpp
  src/pipeline/pipeline.cpp
  src/scoring/scoring.cpp
  src/server/ad_request_handler.cpp
  src/server/http_server.cpp
  src/state/redis_client.cpp
  src/state/runtime_state.cpp
)
target_include_directories(ad_core PUBLIC src)
target_link_libraries(ad_core PUBLIC Threads::Threads)
if(WIN32)
  target_link_libraries(ad_core PUBLIC ws2_32)
  target_compile_definitions(ad_core PUBLIC NOMINMAX)
endif()
if(NOT MSVC)
  target_compile_options(ad_core PUBLIC -Wall -Wextra)
endif()

add_executable(ad_engine src/main.cpp)
target_link_libraries(ad_engine PRIVATE ad_core)

add_executable(ad_engine_tests
  tests/test_main.cpp
  tests/test_common.cpp
  tests/test_catalog.cpp
  tests/test_state.cpp
  tests/test_auction.cpp
  tests/test_events.cpp
  tests/test_end_to_end.cpp
)
target_link_libraries(ad_engine_tests PRIVATE ad_core)
target_compile_definitions(ad_engine_tests PRIVATE
  AD_TEST_DATA_DIR="${CMAKE_CURRENT_BINARY_DIR}"
  AD_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

enable_testing()
add_test(NAME ad_engine_tests COMMAND ad_engine_tests)
