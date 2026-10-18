cmake_minimum_required(VERSION 3.18)
project(ping_native_tests C)
enable_testing()
# Compile the real CLI with deterministic DNS/ICMP substitutes, without a
# DMOD loader or network. Keep this build independent of module downloads.
add_executable(ping_dns_test ping_dns_test.c)
target_include_directories(ping_dns_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(ping_dns_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME ping_dns COMMAND ping_dns_test)
