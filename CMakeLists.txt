cmake_minimum_required(VERSION 3.18)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT DEFINED DMOD_MODULE_VERSION)
    set(DMOD_MODULE_VERSION "0.1" CACHE STRING "DMOD module version")
endif()

# If a local dmod checkout is provided, use it instead of fetching from git
if(DEFINED DMOD_DIR AND NOT "${DMOD_DIR}" STREQUAL "")
    set(FETCHCONTENT_SOURCE_DIR_DMOD "${DMOD_DIR}" CACHE PATH "" FORCE)
endif()

include(FetchContent)
FetchContent_Declare(
    dmod
    GIT_REPOSITORY https://github.com/choco-technologies/dmod.git
    GIT_TAG        develop
)

set(DMOD_MODE "DMOD_MODULE" CACHE STRING "DMOD build mode")
set(DMOD_BUILD_TESTS OFF CACHE BOOL "Build tests")
set(DMOD_BUILD_EXAMPLES OFF CACHE BOOL "Build examples")
set(DMOD_BUILD_TOOLS OFF CACHE BOOL "Build tools")
set(DMOD_BUILD_TEMPLATES OFF CACHE BOOL "Build templates")

FetchContent_MakeAvailable(dmod)

project(dmicmp VERSION ${DMOD_MODULE_VERSION}
    DESCRIPTION "dmicmp DMOD library module"
    LANGUAGES C CXX)

set(DMOD_DIR ${dmod_SOURCE_DIR} CACHE PATH "DMOD source directory")
set(DMOD_SCRIPTS_DIR ${DMOD_DIR}/scripts CACHE PATH "DMOD scripts directory")
include(${DMOD_DIR}/paths.cmake)
dmod_setup_external_module()

# Name of the module
set(DMOD_MODULE_NAME        dmicmp)

# Author (should be string)
set(DMOD_AUTHOR_NAME        "Patryk Kubiak")

# Stack size for the module (should be integer)
set(DMOD_STACK_SIZE         1024)

# Path to the .dmr file - enables release package creation in build/packages/
set(DMOD_DMR_PATH           ${CMAKE_CURRENT_SOURCE_DIR}/dmicmp.dmr)

# List of MAL interfaces implemented by this module (leave empty if none)
set(DMOD_MAL_IMPLS          )

# List of DIF interfaces implemented by this module (leave empty if none)
set(DMOD_DIF_IMPLS          )

#
#   dmod_add_library - create a library module
#   it has the same signature as add_library
#   and can be used in the same way after the creation
#   (for example, to link libraries)
#
dmod_add_library(${DMOD_MODULE_NAME} ${DMOD_MODULE_VERSION}
    # List of source files - can include C and C++ files
    src/dmicmp.c
)

target_include_directories(${DMOD_MODULE_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_include_directories(${DMOD_MODULE_NAME}_if INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# dmip: dmip_send()/_v4_send() do the actual work of transmitting a built
#       ICMP message - dmicmp only builds/parses/checksums the ICMP
#       message itself and hands it to dmip, plus dmip_checksum(),
#       dmip_v4_parse_header() (to read the original IP header out of an
#       unclaimed/undeliverable packet), and dmip_register_protocol()/
#       _register_default_protocol() to receive completed IP packets
#       (see src/dmicmp.c).
# dmroute: header-only - dmip.h needs dmroute_addr_t (re-exported as
#          dmip_addr_t) directly, even though dmicmp.c itself never calls
#          a dmroute_* function (same reason dmudp/dmip link it - see
#          their own CMakeLists.txt).
# dmnetif: dmnetif_iface_t, the iface parameter dmip_protocol_handler_t
#          passes to dmicmp_handle_ip_packet() - dmicmp never calls a
#          dmnetif_* function itself either.
# dmlist:  backs the echo listener registry
# dmosi:   mutex guarding that registry
dmod_link_modules(${DMOD_MODULE_NAME}
    dmip
    dmroute
    dmnetif
    dmlist
    dmosi
)

enable_testing()
add_subdirectory(tests)
