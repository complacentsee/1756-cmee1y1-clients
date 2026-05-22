# CMakeLists.txt for libbpclient (C) — outbound tag I/O for 1756-CMEE1Y1.
#
# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.12)
project(bpclient C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_options(
    -Wall -Wextra -Wshadow -Wpointer-arith -Wstrict-prototypes
    -Wno-unused-parameter
    -fPIC
)

include_directories(include)

set(LIB_SOURCES
    src/client.c
    src/tagdb.c
    src/access.c
    src/errors.c
)

# Shared library
add_library(bpclient SHARED ${LIB_SOURCES})
target_link_libraries(bpclient PRIVATE pthread rt)
set_target_properties(bpclient PROPERTIES
    PUBLIC_HEADER include/bpclient.h
    SOVERSION 0
    VERSION 0.1.0
)

# Static library (same sources, different target)
add_library(bpclient_static STATIC ${LIB_SOURCES})
set_target_properties(bpclient_static PROPERTIES OUTPUT_NAME bpclient)
target_link_libraries(bpclient_static PRIVATE pthread rt)

# Example: tagtest — canonical smoke test (read/write OCX_TEST)
add_executable(tagtest examples/tagtest.c)
target_link_libraries(tagtest PRIVATE bpclient pthread rt)

# Example: discover — enumerate symbols on a PLC, group by type
add_executable(discover examples/discover.c)
target_link_libraries(discover PRIVATE bpclient pthread rt)

# Example: dumpsym — full symbol-info dump for diagnostic purposes
add_executable(dumpsym examples/dumpsym.c)
target_link_libraries(dumpsym PRIVATE bpclient pthread rt)

# Example: typetest — comprehensive type+array+string round-trip tests
add_executable(typetest examples/typetest.c)
target_link_libraries(typetest PRIVATE bpclient pthread rt m)

# Install
include(GNUInstallDirs)
install(TARGETS bpclient
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(TARGETS bpclient_static
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(TARGETS tagtest
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
