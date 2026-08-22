cmake_minimum_required(VERSION 3.20)
project(high_performance_api_gateway CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wpedantic -Wshadow)

option(ENABLE_SANITIZERS "Build with ASan+UBSan" OFF)
if(ENABLE_SANITIZERS)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address,undefined)
endif()

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Core gateway library (everything except main.cpp), so both the gateway
# binary and the test binary can link against the same compiled objects.
# ---------------------------------------------------------------------------
add_library(gateway_lib
    src/http/HttpParser.cpp
    src/core/EventLoop.cpp
    src/core/Gateway.cpp
    src/network/TcpServer.cpp
    src/network/ConnectionPool.cpp
    src/network/HealthChecker.cpp
    src/config/Config.cpp
)
target_include_directories(gateway_lib PUBLIC include)
target_link_libraries(gateway_lib PUBLIC Threads::Threads)

add_executable(gateway src/main.cpp)
target_link_libraries(gateway PRIVATE gateway_lib)

add_executable(backend_server backends/backend_server.cpp)
target_link_libraries(backend_server PRIVATE Threads::Threads)

add_executable(gateway_benchmark benchmarks/gateway_benchmark.cpp)
target_link_libraries(gateway_benchmark PRIVATE Threads::Threads)

# ---------------------------------------------------------------------------
# Tests (GoogleTest, fetched from GitHub at configure time)
# ---------------------------------------------------------------------------
option(BUILD_TESTS "Build unit/integration tests" ON)
if(BUILD_TESTS)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
  )
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  enable_testing()
  add_executable(gateway_tests
    tests/http_parser_tests.cpp
    tests/router_tests.cpp
    tests/rate_limiter_tests.cpp
    tests/cache_tests.cpp
    tests/circuit_breaker_tests.cpp
    tests/retry_policy_tests.cpp
    tests/integration_tests.cpp
  )
  target_link_libraries(gateway_tests PRIVATE gateway_lib gtest_main)
  include(GoogleTest)
  gtest_discover_tests(gateway_tests)
endif()
