cmake_minimum_required(VERSION 3.16)
project(neato-host-tests CXX)

# Host tests for the parts of the firmware that have no ESP-IDF dependency.
#
# Deliberately a separate project rather than an IDF component: these run on a
# developer's machine and in CI with nothing but a C++ compiler, which is the
# only reason they get run at all. Build and run with:
#
#   cmake -S test -B build-test && cmake --build build-test
#   ctest --test-dir build-test --output-on-failure

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

add_executable(host_tests
    test_main.cpp
    ../main/text_util.cpp
    ../main/dns_message.cpp
)
target_include_directories(host_tests PRIVATE ../main)
target_compile_options(host_tests PRIVATE -Wall -Wextra -Werror)

add_test(NAME host_tests COMMAND host_tests)
