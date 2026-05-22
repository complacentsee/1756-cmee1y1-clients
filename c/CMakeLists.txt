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
    src/message.c
    src/identity.c
    src/module.c
)

# Shared library
add_library(bpclient SHARED ${LIB_SOURCES})
target_link_libraries(bpclient PRIVATE pthread rt)
set_target_properties(bpclient PROPERTIES
    PUBLIC_HEADER include/bpclient.h
    SOVERSION 0
    VERSION 0.4.5
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

# Example: findtags — print all symbols matching a name prefix
add_executable(findtags examples/findtags.c)
target_link_libraries(findtags PRIVATE bpclient pthread rt)

# Example: structprobe — raw-byte dump of OCXcip_GetStructInfo +
# OCXcip_GetStructMbrInfo responses (diagnostic / RE tool)
add_executable(structprobe examples/structprobe.c)
target_link_libraries(structprobe PRIVATE bpclient pthread rt)

# Example: symprobe — raw-byte dump of OCXcip_GetSymbolInfo with an
# oversized payload window (diagnostic / RE tool — used to characterize
# 3-D array layout)
add_executable(symprobe examples/symprobe.c)
target_link_libraries(symprobe PRIVATE bpclient pthread rt)

# Example: testtagver — validate bp_tagdb_test_version() flow
add_executable(testtagver examples/testtagver.c)
target_link_libraries(testtagver PRIVATE bpclient pthread rt)

# Example: msgprobe — manual OCXcip_MessageSend invocation (diagnostic /
# RE tool — used to characterize Phase 3 wire format)
add_executable(msgprobe examples/msgprobe.c)
target_link_libraries(msgprobe PRIVATE bpclient pthread rt)

# Example: identity — query a device's CIP Identity via Get_Attribute_All
add_executable(identity examples/identity.c)
target_link_libraries(identity PRIVATE bpclient pthread rt)

# Example: pathprobe — call OCXcip_ParsePath and dump the canonical
# encoded EPATH bytes (diagnostic / verification tool)
add_executable(pathprobe examples/pathprobe.c)
target_link_libraries(pathprobe PRIVATE bpclient pthread rt)

# Example: actnodes — dump the OCXcip_GetActiveNodeTable bitmap
add_executable(actnodes examples/actnodes.c)
target_link_libraries(actnodes PRIVATE bpclient pthread rt)

# Example: msgprobe_routed — MessageSend with a pre-opened TagDb handle
# (diagnostic: tests whether routing is inherited from CreateTagDbHandle)
add_executable(msgprobe_routed examples/msgprobe_routed.c)
target_link_libraries(msgprobe_routed PRIVATE bpclient pthread rt)

# Example: modutil — local cm1756 module utilities (switch / display / LED)
add_executable(modutil examples/modutil.c)
target_link_libraries(modutil PRIVATE bpclient pthread rt)

# Example: udtinfo — dump a UDT's full schema using the public API
add_executable(udtinfo examples/udtinfo.c)
target_link_libraries(udtinfo PRIVATE bpclient pthread rt)

# Install
include(GNUInstallDirs)
install(TARGETS bpclient
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(TARGETS bpclient_static
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(TARGETS
            tagtest
            discover
            dumpsym
            typetest
            findtags
            structprobe
            symprobe
            testtagver
            msgprobe
            msgprobe_routed
            identity
            pathprobe
            actnodes
            modutil
            udtinfo
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
